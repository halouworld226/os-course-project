#include "scheduler.hpp"
#include "worker.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {

std::string event_json(const char* name, const std::string& job_id, const Tile& tile,
                       unsigned attempt, unsigned long pid) {
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream json;
    json << "{\"event\":\"" << name << "\",\"job_id\":\"" << job_id << "\",\"tile_id\":" << tile.id
         << ",\"attempt_id\":" << attempt << ",\"worker_pid\":" << pid << ",\"timestamp_ms\":" << timestamp
         << ",\"x\":" << tile.x << ",\"y\":" << tile.y << ",\"width\":" << tile.width
         << ",\"height\":" << tile.height << '}';
    return json.str();
}

} // namespace

SchedulerResult run_tiles(const std::filesystem::path& executable, const Image& source,
                          Operation operation, const std::string& job_id,
                          const TileCommit& commit, const SchedulerOptions& options) {
    if (options.worker_count == 0 || options.worker_count > 16) throw std::runtime_error("worker count must be 1..16");
    const auto tiles = partition(source);
    SchedulerResult result{{source.width, source.height, Bytes(source.pixels.size())}, {}};
    std::vector<std::unique_ptr<Worker>> workers;
    for (unsigned i = 0; i < options.worker_count; ++i) workers.push_back(std::make_unique<Worker>(executable));

    // Reserve one initial tile for each worker so a fast worker cannot consume the
    // whole small image before the other worker threads enter their loops.
    const auto initial = std::min<std::size_t>(workers.size(), tiles.size());
    std::deque<unsigned> ready;
    for (std::size_t i = initial; i < tiles.size(); ++i) ready.push_back(static_cast<unsigned>(i));
    std::vector<unsigned> attempts(tiles.size(), 0);
    std::vector<bool> committed(tiles.size(), false);
    std::mutex state_mutex;
    std::exception_ptr failure;
    bool injected = false;

    auto emit = [&](const char* name, const Tile& tile, unsigned attempt, unsigned long pid) {
        result.events.push_back(event_json(name, job_id, tile, attempt, pid));
        if (options.on_event) options.on_event(result.events.back());
    };

    std::vector<std::thread> threads;
    auto process_slot = [&](std::size_t slot) {
        try {
            bool first = true;
            for (;;) {
                unsigned tile_id;
                unsigned attempt;
                bool kill_this_attempt = false;
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    if (failure) break;
                    if (first && slot < initial) {
                        tile_id = static_cast<unsigned>(slot);
                    } else {
                        if (ready.empty()) break;
                        tile_id = ready.front();
                        ready.pop_front();
                    }
                    first = false;
                    attempt = ++attempts[tile_id];
                    if (options.inject_failure && !injected) {
                        injected = true;
                        kill_this_attempt = true;
                    }
                    emit("tile_dispatched", tiles[tile_id], attempt, workers[slot]->pid());
                }

                const auto& tile = tiles[tile_id];
                Bytes pixels;
                try {
                    pixels = workers[slot]->process(operation, extract_tile(source, tile), kill_this_attempt);
                    if (pixels.size() != static_cast<std::size_t>(tile.width) * tile.height * 3) {
                        throw std::runtime_error("worker returned an invalid tile length");
                    }
                } catch (...) {
                    const auto old_pid = workers[slot]->pid();
                    // Closing the pipes and reaping happen before replacement, so
                    // the failed child cannot retain resources or submit later.
                    workers[slot].reset();
                    {
                        std::lock_guard<std::mutex> lock(state_mutex);
                        emit("worker_exit", tile, attempt, old_pid);
                        if (attempt >= 3) throw std::runtime_error("tile failed after three attempts");
                        ready.push_back(tile_id);
                        emit("tile_requeued", tile, attempt, old_pid);
                    }
                    workers[slot] = std::make_unique<Worker>(executable);
                    continue;
                }

                std::lock_guard<std::mutex> lock(state_mutex);
                if (failure) break;
                if (committed[tile_id]) throw std::runtime_error("duplicate tile commit");
                commit(tile, pixels);
                place_tile(result.image, tile, pixels);
                committed[tile_id] = true;
                emit("tile_committed", tile, attempt, workers[slot]->pid());
            }
            if (workers[slot]) workers[slot]->finish();
        } catch (...) {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!failure) failure = std::current_exception();
        }
    };

    try {
        for (std::size_t slot = 0; slot < workers.size(); ++slot) threads.emplace_back(process_slot, slot);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            failure = std::current_exception();
        }
        for (auto& thread : threads) thread.join();
        throw;
    }
    for (auto& thread : threads) thread.join();
    if (failure) std::rethrow_exception(failure);
    if (!std::all_of(committed.begin(), committed.end(), [](bool value) { return value; })) {
        throw std::runtime_error("scheduler ended with incomplete tiles");
    }
    return result;
}
