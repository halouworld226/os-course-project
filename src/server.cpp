#include "image.hpp"
#include "net.hpp"
#include "storage.hpp"
#include "worker.hpp"
#include "scheduler.hpp"
#include "job_scheduler.hpp"

#include <charconv>
#include <chrono>
#include <csignal>
#include <iostream>
#include <algorithm>
#include <future>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace {

struct LiveJob {
    std::string operation;
    std::string priority;
    unsigned total = 0;
    unsigned complete = 0;
    std::vector<std::string> events;
    std::string state = "queued";
    unsigned width = 0;
    unsigned height = 0;
    std::string owner;
};

std::string snapshot(const std::string& id, const LiveJob& job, bool complete) {
    std::ostringstream json;
    json << "{\"job_id\":\"" << id << "\",\"operation\":\"" << job.operation
         << "\",\"priority\":\"" << job.priority << "\",\"state\":\"" << (complete ? "complete" : job.state)
         << "\",\"width\":" << job.width << ",\"height\":" << job.height
         << ",\"tile_total\":" << job.total << ",\"tile_complete\":" << job.complete << ",\"events\":[";
    for (std::size_t i = 0; i < job.events.size(); ++i) {
        if (i) json << ',';
        json << job.events[i];
    }
    json << "]}";
    return json.str();
}

class JobService {
public:
    JobService(JobStore& store, const std::filesystem::path& executable)
        : store_(store), scheduler_(std::make_unique<SharedScheduler>(executable, 4)) {}

    ~JobService() {
        for (auto& task : background_) task.wait();
        scheduler_.reset();
    }

    std::string submit(Operation operation, const Bytes& source, bool high, bool inject_failure, bool wait,
                       const std::string& owner) {
        std::unique_lock<std::mutex> submit_lock(submit_mutex_);
        background_.erase(std::remove_if(background_.begin(), background_.end(), [](auto& task) {
            return task.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        }), background_.end());
        if (background_.size() >= 8) throw std::runtime_error("eight jobs already outstanding");
        const auto image = decode_ppm(source);
        const auto tiles = partition(image);
        const std::string name = operation == Operation::grayscale ? "grayscale" : "invert";
        const std::string priority = high ? "high" : "normal";
        std::ostringstream description;
        description << "{\"operation\":\"" << name << "\",\"priority\":\"" << priority << "\",\"width\":" << image.width
                    << ",\"height\":" << image.height << ",\"tile_total\":" << tiles.size() << '}';
        const auto id = store_.create_job(source, description.str(), owner);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.emplace(id, LiveJob{name, priority, static_cast<unsigned>(tiles.size()), 0, {}, "queued",
                                      image.width, image.height, owner});
        }
        try {
            auto future = scheduler_->submit(id, image, operation, high,
                [this, id](const Tile& tile, const Bytes& pixels) { store_.save_tile(id, tile.id, pixels); },
                [this, id](const std::string& event) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto& job = jobs_.at(id);
                    job.state = "running";
                    job.events.push_back(event);
                    if (event.find("\"event\":\"tile_committed\"") != std::string::npos) ++job.complete;
                    std::cerr << event << '\n';
                }, inject_failure);
            auto task = std::async(std::launch::async, [this, id, result_future = std::move(future)]() mutable {
                try {
                    const auto result = result_future.get();
                    std::string status;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        status = snapshot(id, jobs_.at(id), true);
                    }
                    store_.finish(id, encode_ppm(result.image), status);
                    std::lock_guard<std::mutex> lock(mutex_);
                    jobs_.erase(id);
                } catch (...) {
                    try { store_.abort(id); }
                    catch (const std::exception& error) { std::cerr << "abort cleanup: " << error.what() << '\n'; }
                    std::lock_guard<std::mutex> lock(mutex_);
                    jobs_.at(id).state = "failed";
                    throw;
                }
            }).share();
            background_.push_back(task);
            submit_lock.unlock();
            if (wait) task.get();
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.at(id).state = "failed";
            throw;
        }
        return id;
    }

    Bytes status(const std::string& id) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = jobs_.find(id);
            if (found != jobs_.end()) {
                const auto json = snapshot(id, found->second, false);
                return Bytes(json.begin(), json.end());
            }
        }
        return store_.status(id);
    }

    void authorize(const std::string& id, const std::string& user) {
        if (user == "admin") return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = jobs_.find(id);
            if (found != jobs_.end()) {
                if (found->second.owner != user) throw std::runtime_error("permission denied");
                return;
            }
        }
        if (store_.owner(id) != user) throw std::runtime_error("permission denied");
    }

    Bytes active() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string json = "{\"jobs\":[";
        bool first = true;
        for (const auto& item : jobs_) {
            if (!first) json += ',';
            first = false;
            json += snapshot(item.first, item.second, false);
        }
        json += "]}";
        return Bytes(json.begin(), json.end());
    }

    bool terminate(unsigned long pid) { return scheduler_->terminate_worker(pid); }

    Bytes queues() {
        const auto json = scheduler_->snapshot();
        return Bytes(json.begin(), json.end());
    }

private:
    JobStore& store_;
    std::unique_ptr<SharedScheduler> scheduler_;
    std::mutex mutex_;
    std::map<std::string, LiveJob> jobs_;
    std::mutex submit_mutex_;
    std::vector<std::shared_future<void>> background_;
};

void serve(RequestStream& client, JobStore& store, JobService& service, bool authentication_required = false) {
    Bytes reply;
    try {
        auto command = client.read_line();
        std::string user = "admin";
        if (command.rfind("AUTH ", 0) == 0) {
            std::istringstream auth(command.substr(5));
            std::string password, extra;
            auth >> user >> password;
            command = client.read_line();
            if ((user != "alice" && user != "bob" && user != "admin") || password != "demo123" || (auth >> extra)) {
                if (command.rfind("SUBMIT ", 0) == 0 || command.rfind("ENQUEUE ", 0) == 0) client.read_payload();
                throw std::runtime_error("invalid credentials");
            }
        } else if (authentication_required) {
            throw std::runtime_error("authentication required");
        }
        if (command.rfind("SUBMIT ", 0) == 0 || command.rfind("ENQUEUE ", 0) == 0) {
            const auto source = client.read_payload();
            const bool wait = command.rfind("SUBMIT ", 0) == 0;
            std::istringstream tokens(command.substr(wait ? 7 : 8));
            std::string operation_name;
            tokens >> operation_name;
            if (operation_name != "GRAYSCALE" && operation_name != "INVERT") throw std::runtime_error("unknown operation");
            bool high = false;
            bool inject_failure = false;
            for (std::string flag; tokens >> flag;) {
                if (flag == "HIGH" && !high) high = true;
                else if (flag == "FAULT" && !inject_failure) inject_failure = true;
                else throw std::runtime_error("invalid submit flag");
            }
            const auto operation = operation_name == "GRAYSCALE" ? Operation::grayscale : Operation::invert;
            const auto id = service.submit(operation, source, high, inject_failure, wait, user);
            reply.assign(id.begin(), id.end());
        } else if (command.rfind("STATUS ", 0) == 0) {
            service.authorize(command.substr(7), user);
            reply = service.status(command.substr(7));
        } else if (command.rfind("GET ", 0) == 0) {
            service.authorize(command.substr(4), user);
            reply = store.download(command.substr(4));
        } else if (command == "ACTIVE") {
            if (user != "admin") throw std::runtime_error("administrator required");
            reply = service.active();
        } else if (command == "QUEUES") {
            if (user != "admin") throw std::runtime_error("administrator required");
            reply = service.queues();
        } else if (command.rfind("KILL ", 0) == 0) {
            if (user != "admin") throw std::runtime_error("administrator required");
            unsigned long pid = 0;
            const auto value = std::string_view(command).substr(5);
            const auto result = std::from_chars(value.data(), value.data() + value.size(), pid);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || !service.terminate(pid)) {
                throw std::runtime_error("PID is not a busy owned worker");
            }
            const std::string json = "{\"terminated\":true}";
            reply.assign(json.begin(), json.end());
        } else if (command.rfind("TILE ", 0) == 0) {
            std::istringstream args(command.substr(5));
            std::string id, number, extra;
            args >> id >> number;
            unsigned tile = 0;
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), tile);
            if (id.empty() || parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || (args >> extra)) {
                throw std::runtime_error("invalid tile request");
            }
            service.authorize(id, user);
            reply = store.tile(id, tile);
        } else {
            throw std::runtime_error("unknown command");
        }
    } catch (const std::exception& error) {
        std::cerr << "request failed: " << error.what() << '\n';
        const std::string message = "request failed; see server log";
        client.write_line("ERR\n");
        client.write_payload(Bytes(message.begin(), message.end()));
        return;
    }
    client.write_line("OK\n");
    client.write_payload(reply);
}

unsigned parse_port(std::string_view value) {
    unsigned port = 0;
    const auto end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, port);
    if (parsed.ec != std::errc{} || parsed.ptr != end || port > 65535) throw std::runtime_error("invalid port");
    return port;
}

} // namespace

int main(int argc, char** argv) {
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif
    if (argc == 2 && std::string_view(argv[1]) == "--worker") return worker_main();
    if (argc != 3) {
        std::cerr << "usage: os_server PORT|--stdio|--session DATA_DIRECTORY\n";
        return 2;
    }
    try {
        const auto executable = std::filesystem::absolute(argv[0]);
        JobStore store(argv[2]);
        JobService service(store, executable);
        if (std::string_view(argv[1]) == "--session") {
            StdioStream client;
            while (client.has_next()) {
                serve(client, store, service);
                client.flush();
            }
            return 0;
        }
        if (std::string_view(argv[1]) == "--stdio") {
            StdioStream client;
            serve(client, store, service);
            client.flush();
            return 0;
        }
        TcpListener listener(parse_port(argv[1]));
        std::cout << "PORT " << listener.port() << std::endl;
        std::vector<std::future<void>> requests;
        for (;;) {
            auto client = listener.accept_client();
            requests.erase(std::remove_if(requests.begin(), requests.end(), [](auto& request) {
                return request.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            }), requests.end());
            if (requests.size() >= 8) {
                client.write_line("ERR\n");
                const std::string message = "server is busy; retry shortly";
                client.write_payload(Bytes(message.begin(), message.end()));
                continue;
            }
            requests.push_back(std::async(std::launch::async, [connection = std::move(client), &store, &service]() mutable {
                try { serve(connection, store, service, true); }
                catch (const std::exception& error) { std::cerr << "client connection: " << error.what() << '\n'; }
            }));
        }
    } catch (const std::exception& error) {
        std::cerr << "server: " << error.what() << '\n';
        return 1;
    }
}
