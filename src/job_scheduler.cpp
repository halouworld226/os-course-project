#include "job_scheduler.hpp"
#include "worker.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

struct SharedScheduler::State {
    struct Job {
        std::string id;
        Image source;
        Operation operation;
        bool high;
        bool inject_failure;
        bool injected = false;
        bool failed = false;
        std::vector<Tile> tiles;
        std::vector<unsigned> attempts;
        std::vector<bool> committed;
        std::size_t remaining;
        TileCommit commit;
        EventCallback on_event;
        SchedulerResult result;
        std::promise<SchedulerResult> promise;
    };

    struct Task {
        std::shared_ptr<Job> job;
        unsigned tile_id;
    };

    struct BusyWorker {
        Worker* process;
        std::string job_id;
        unsigned tile_id;
        unsigned attempt;
    };

    std::filesystem::path executable;
    std::vector<std::unique_ptr<Worker>> workers;
    std::vector<std::thread> threads;
    std::deque<Task> high_queue;
    std::deque<Task> normal_queue;
    std::mutex mutex;
    std::condition_variable changed;
    std::unordered_map<unsigned long, BusyWorker> busy;
    bool stopping = false;

    explicit State(std::filesystem::path path, unsigned count) : executable(std::move(path)) {
        if (count == 0 || count > 16) throw std::runtime_error("worker count must be 1..16");
        for (unsigned slot = 0; slot < count; ++slot) workers.push_back(std::make_unique<Worker>(executable));
        try {
            for (unsigned slot = 0; slot < count; ++slot) threads.emplace_back([this, slot] { run_slot(slot); });
        } catch (...) {
            shutdown();
            throw;
        }
    }

    void shutdown() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        changed.notify_all();
        for (auto& thread : threads) if (thread.joinable()) thread.join();
    }

    ~State() { shutdown(); }

    void emit(Job& job, const char* name, unsigned tile_id, unsigned attempt, unsigned long pid) {
        const auto& tile = job.tiles[tile_id];
        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::ostringstream json;
        json << "{\"event\":\"" << name << "\",\"job_id\":\"" << job.id << "\",\"tile_id\":" << tile.id
             << ",\"attempt_id\":" << attempt << ",\"worker_pid\":" << pid << ",\"timestamp_ms\":" << timestamp
             << ",\"x\":" << tile.x << ",\"y\":" << tile.y << ",\"width\":" << tile.width
             << ",\"height\":" << tile.height << ",\"priority\":\"" << (job.high ? "high" : "normal") << "\"}";
        job.result.events.push_back(json.str());
        if (job.on_event) job.on_event(job.result.events.back());
    }

    void fail(Job& job, std::exception_ptr error) {
        if (job.failed) return;
        job.failed = true;
        job.promise.set_exception(error);
    }

    std::deque<Task>& queue(bool high) { return high ? high_queue : normal_queue; }

    void run_slot(unsigned slot) noexcept {
        for (;;) {
            Task task;
            unsigned attempt = 0;
            bool inject = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&] { return stopping || !high_queue.empty() || !normal_queue.empty(); });
                if (high_queue.empty() && normal_queue.empty()) break;
                auto& ready = high_queue.empty() ? normal_queue : high_queue;
                task = std::move(ready.front());
                ready.pop_front();
                if (task.job->failed) continue;
                auto& job = *task.job;
                attempt = ++job.attempts[task.tile_id];
                inject = job.inject_failure && !job.injected;
                if (inject) job.injected = true;
                try {
                    if (!workers[slot]) workers[slot] = std::make_unique<Worker>(executable);
                    busy.emplace(workers[slot]->pid(), BusyWorker{workers[slot].get(), job.id, task.tile_id, attempt});
                    emit(job, "tile_dispatched", task.tile_id, attempt, workers[slot]->pid());
                } catch (...) {
                    if (workers[slot]) busy.erase(workers[slot]->pid());
                    fail(job, std::current_exception());
                    continue;
                }
            }

            auto& job = *task.job;
            const auto& tile = job.tiles[task.tile_id];
            try {
                Bytes pixels;
                try {
                    pixels = workers[slot]->process(job.operation, extract_tile(job.source, tile), inject);
                    if (pixels.size() != static_cast<std::size_t>(tile.width) * tile.height * 3) {
                        throw std::runtime_error("worker returned an invalid tile length");
                    }
                } catch (...) {
                    const auto pid = workers[slot]->pid();
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        busy.erase(pid);
                    }
                    workers[slot].reset();
                    std::lock_guard<std::mutex> lock(mutex);
                    if (job.failed) continue;
                    emit(job, "worker_exit", task.tile_id, attempt, pid);
                    if (attempt >= 3) throw std::runtime_error("tile failed after three attempts");
                    queue(job.high).push_back(task);
                    emit(job, "tile_requeued", task.tile_id, attempt, pid);
                    changed.notify_all();
                    continue;
                }

                std::lock_guard<std::mutex> lock(mutex);
                busy.erase(workers[slot]->pid());
                if (job.failed) continue;
                if (job.committed[task.tile_id]) throw std::runtime_error("duplicate tile commit");
                job.commit(tile, pixels);
                place_tile(job.result.image, tile, pixels);
                job.committed[task.tile_id] = true;
                emit(job, "tile_committed", task.tile_id, attempt, workers[slot]->pid());
                if (--job.remaining == 0) job.promise.set_value(std::move(job.result));
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex);
                if (workers[slot]) busy.erase(workers[slot]->pid());
                fail(job, std::current_exception());
            }
        }
        workers[slot].reset();
    }
};

SharedScheduler::SharedScheduler(const std::filesystem::path& executable, unsigned workers)
    : state_(std::make_unique<State>(executable, workers)) {}

SharedScheduler::~SharedScheduler() = default;

bool SharedScheduler::terminate_worker(unsigned long pid) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto found = state_->busy.find(pid);
    if (found == state_->busy.end()) return false;
    found->second.process->terminate();
    return true;
}

std::vector<unsigned long> SharedScheduler::busy_pids() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    std::vector<unsigned long> result;
    for (const auto& entry : state_->busy) result.push_back(entry.first);
    return result;
}

std::string SharedScheduler::snapshot() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    std::ostringstream json;
    const auto write_queue = [&](const std::deque<State::Task>& queue) {
        json << '[';
        bool comma = false;
        for (const auto& task : queue) {
            if (task.job->failed) continue;
            if (comma) json << ',';
            comma = true;
            json << "{\"job_id\":\"" << task.job->id << "\",\"tile_id\":" << task.tile_id << '}';
        }
        json << ']';
    };
    json << "{\"high_queue\":";
    write_queue(state_->high_queue);
    json << ",\"normal_queue\":";
    write_queue(state_->normal_queue);
    json << ",\"workers\":[";
    bool comma = false;
    for (const auto& entry : state_->busy) {
        if (comma) json << ',';
        comma = true;
        const auto& worker = entry.second;
        json << "{\"pid\":" << entry.first << ",\"job_id\":\"" << worker.job_id << "\",\"tile_id\":"
             << worker.tile_id << ",\"attempt_id\":" << worker.attempt << '}';
    }
    json << "]}";
    return json.str();
}

std::future<SchedulerResult> SharedScheduler::submit(std::string job_id, Image source, Operation operation,
                                                    bool high, TileCommit commit, EventCallback on_event,
                                                    bool inject_failure) {
    if (source.width == 0 || source.height == 0 || source.width > max_image_edge || source.height > max_image_edge ||
        source.pixels.size() != static_cast<std::size_t>(source.width) * source.height * 3 || !commit) {
        throw std::runtime_error("invalid image or tile commit callback");
    }
    if (job_id.empty() || job_id.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_") != std::string::npos) {
        throw std::runtime_error("invalid scheduler job ID");
    }
    auto job = std::make_shared<State::Job>();
    job->id = std::move(job_id);
    job->source = std::move(source);
    job->operation = operation;
    job->high = high;
    job->inject_failure = inject_failure;
    job->tiles = partition(job->source);
    job->remaining = job->tiles.size();
    job->attempts.resize(job->remaining, 0);
    job->committed.resize(job->remaining, false);
    job->commit = std::move(commit);
    job->on_event = std::move(on_event);
    job->result.image = {job->source.width, job->source.height, Bytes(job->source.pixels.size())};
    auto future = job->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->stopping) throw std::runtime_error("scheduler is shutting down");
        auto& ready = state_->queue(high);
        const auto old_size = ready.size();
        try {
            for (std::size_t tile = 0; tile < job->tiles.size(); ++tile) {
                ready.push_back({job, static_cast<unsigned>(tile)});
            }
        } catch (...) {
            while (ready.size() > old_size) ready.pop_back();
            throw;
        }
    }
    state_->changed.notify_all();
    return future;
}
