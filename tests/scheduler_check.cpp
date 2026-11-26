#include "scheduler.hpp"
#include "worker.hpp"

#include <algorithm>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string_view>

int main(int argc, char** argv) {
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif
    if (argc == 2 && std::string_view(argv[1]) == "--worker") return worker_main();
    try {
        Image source{130, 67, Bytes(130 * 67 * 3)};
        for (std::size_t i = 0; i < source.pixels.size(); ++i) source.pixels[i] = static_cast<std::uint8_t>(i * 17);
        for (const auto operation : {Operation::grayscale, Operation::invert}) {
            for (const bool inject : {false, true}) {
                std::set<unsigned> committed;
                std::vector<std::string> live_events;
                SchedulerOptions options;
                options.inject_failure = inject;
                options.on_event = [&](const std::string& event) { live_events.push_back(event); };
                const auto result = run_tiles(std::filesystem::absolute(argv[0]), source, operation, "test-0",
                    [&](const Tile& tile, const Bytes&) {
                        if (!committed.insert(tile.id).second) throw std::runtime_error("tile committed twice");
                    }, options);
                auto expected = source.pixels;
                process_pixels(operation, expected);
                if (result.image.pixels != expected || committed.size() != 6 || live_events != result.events) {
                    throw std::runtime_error("image, commit count or live event mismatch");
                }
                const auto count_event = [&](const std::string& name) {
                    return std::count_if(result.events.begin(), result.events.end(), [&](const std::string& json) {
                        return json.find("\"event\":\"" + name + "\"") != std::string::npos;
                    });
                };
                if (count_event("tile_committed") != 6 || count_event("tile_dispatched") != (inject ? 7 : 6) ||
                    count_event("worker_exit") != (inject ? 1 : 0) || count_event("tile_requeued") != (inject ? 1 : 0)) {
                    throw std::runtime_error("fault recovery event counts mismatch");
                }
                std::set<std::string> worker_pids;
                for (const auto& json : result.events) {
                    const auto start = json.find("\"worker_pid\":") + 13;
                    worker_pids.insert(json.substr(start, json.find(',', start) - start));
                }
                if (worker_pids.size() < 4) throw std::runtime_error("four worker processes did not receive tiles");
                std::cout << "PASS operation=" << static_cast<unsigned>(operation) << " injected=" << inject
                          << " tiles=" << committed.size() << " worker_pids=" << worker_pids.size() << '\n';
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
