#include "job_scheduler.hpp"
#include "worker.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {

Image image(unsigned width, unsigned height) {
    Image source{width, height, Bytes(static_cast<std::size_t>(width) * height * 3)};
    for (std::size_t i = 0; i < source.pixels.size(); ++i) source.pixels[i] = static_cast<std::uint8_t>(i * 17);
    return source;
}

void verify(const SchedulerResult& result, const Image& source, Operation operation) {
    auto reference = source.pixels;
    process_pixels(operation, reference);
    if (result.image.pixels != reference) throw std::runtime_error("shared scheduler pixels differ from reference");
    const auto commits = std::count_if(result.events.begin(), result.events.end(), [](const std::string& event) {
        return event.find("\"event\":\"tile_committed\"") != std::string::npos;
    });
    if (static_cast<std::size_t>(commits) != partition(source).size()) throw std::runtime_error("invalid commit count");
}

} // namespace

int main(int argc, char** argv) {
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif
    if (argc == 2 && std::string_view(argv[1]) == "--worker") return worker_main();
    try {
        const auto executable = std::filesystem::absolute(argv[0]);
        const auto normal_source = image(2048, 2048);
        const auto high_source = image(130, 67);
        std::atomic<bool> normal_started{false};
        std::vector<std::string> dispatch_order;
        auto on_event = [&](const std::string& event) {
            if (event.find("\"event\":\"tile_dispatched\"") != std::string::npos) {
                dispatch_order.push_back(event.find("\"job_id\":\"high\"") != std::string::npos ? "high" : "normal");
                normal_started = true;
            }
        };
        auto commit = [](const Tile&, const Bytes&) {};
        {
            SharedScheduler scheduler(executable);
            if (scheduler.snapshot() != "{\"high_queue\":[],\"normal_queue\":[],\"workers\":[]}") {
                throw std::runtime_error("initial scheduler snapshot is not empty");
            }
            auto normal = scheduler.submit("normal", normal_source, Operation::grayscale, false, commit, on_event);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!normal_started) {
                if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("normal dispatch timed out");
                std::this_thread::yield();
            }
            auto high = scheduler.submit("high", high_source, Operation::invert, true, commit, on_event, true);
            const auto snapshot = scheduler.snapshot();
            if (snapshot.find("\"high_queue\":[") == std::string::npos ||
                snapshot.find("\"normal_queue\":[") == std::string::npos ||
                snapshot.find("\"workers\":[") == std::string::npos) {
                throw std::runtime_error("scheduler snapshot is missing queue or worker data");
            }
            verify(high.get(), high_source, Operation::invert);
            verify(normal.get(), normal_source, Operation::grayscale);
            const auto first_high = std::find(dispatch_order.begin(), dispatch_order.end(), "high");
            const auto high_dispatches = std::count(dispatch_order.begin(), dispatch_order.end(), "high");
            if (first_high == dispatch_order.end() || first_high == dispatch_order.begin() || high_dispatches != 7) {
                throw std::runtime_error("expected normal work then seven high dispatches including retry");
            }
            if (std::count(first_high, dispatch_order.end(), "normal") == 0) {
                throw std::runtime_error("normal work finished before high job arrived");
            }
            // The six initial high tasks are selected in FIFO order. A failed
            // tile rejoins the tail at high priority when its exit is detected.
            if (!std::all_of(first_high, first_high + 6, [](const std::string& item) { return item == "high"; })) {
                throw std::runtime_error("normal tile dispatched while the initial high queue was nonempty");
            }
            auto failed = scheduler.submit("bad-store", high_source, Operation::invert, false,
                [](const Tile&, const Bytes&) { throw std::runtime_error("simulated disk full"); }, {});
            try {
                failed.get();
                throw std::runtime_error("storage failure did not reach future");
            } catch (const std::runtime_error& error) {
                if (std::string(error.what()) != "simulated disk full") throw;
            }
            auto after = scheduler.submit("after-error", high_source, Operation::grayscale, false, commit, {});
            verify(after.get(), high_source, Operation::grayscale);
            if (scheduler.terminate_worker(0)) throw std::runtime_error("unowned PID was accepted");
            auto admin = scheduler.submit("admin-stop", normal_source, Operation::invert, false, commit, {});
            bool terminated = false;
            const auto kill_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!terminated && std::chrono::steady_clock::now() < kill_deadline) {
                for (const auto pid : scheduler.busy_pids()) {
                    if (scheduler.terminate_worker(pid)) {
                        terminated = true;
                        break;
                    }
                }
                std::this_thread::yield();
            }
            if (!terminated) throw std::runtime_error("no busy owned worker could be terminated");
            const auto admin_result = admin.get();
            verify(admin_result, normal_source, Operation::invert);
            if (scheduler.snapshot() != "{\"high_queue\":[],\"normal_queue\":[],\"workers\":[]}") {
                throw std::runtime_error("completed jobs remain in scheduler snapshot");
            }
            if (std::none_of(admin_result.events.begin(), admin_result.events.end(), [](const std::string& event) {
                return event.find("\"event\":\"worker_exit\"") != std::string::npos;
            })) throw std::runtime_error("admin termination did not produce a recovery event");
        }
        std::cout << "PASS shared pool: normal/high FIFO dispatch, recovery at original priority, two exact images, "
                     "storage failure propagation, subsequent job completion, and admin busy-worker termination\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
