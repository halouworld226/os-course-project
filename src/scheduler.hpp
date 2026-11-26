#pragma once

#include "image.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct SchedulerOptions {
    unsigned worker_count = 4;
    bool inject_failure = false;
    std::function<void(const std::string&)> on_event;
};

struct SchedulerResult {
    Image image;
    std::vector<std::string> events;
};

using TileCommit = std::function<void(const Tile&, const Bytes&)>;

SchedulerResult run_tiles(const std::filesystem::path& executable, const Image& source,
                          Operation operation, const std::string& job_id,
                          const TileCommit& commit, const SchedulerOptions& options = {});
