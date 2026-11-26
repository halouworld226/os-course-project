#include "storage.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

JobStore::JobStore(std::filesystem::path root) : disk_(root / "disk.img") {
    disk_.mkdir("/jobs");
}

std::string JobStore::job_path(const std::string& job_id) const {
    if (job_id.empty() || job_id.size() > 40 || !std::all_of(job_id.begin(), job_id.end(), [](char c) {
        return (c >= '0' && c <= '9') || c == '-';
    })) throw std::runtime_error("invalid job ID");
    return "/jobs/" + job_id;
}

std::string JobStore::create_job(const Bytes& source, const std::string& job_description, const std::string& owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner != "alice" && owner != "bob" && owner != "admin") throw std::runtime_error("unknown job owner");
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (unsigned sequence = 0; sequence < 1000; ++sequence) {
        const auto id = std::to_string(stamp) + '-' + std::to_string(sequence);
        const auto path = job_path(id);
        if (disk_.exists(path)) continue;
        disk_.mkdir(path);
        try {
            disk_.mkdir(path + "/tiles");
            disk_.write(path + "/source.ppm", source);
            disk_.write(path + "/job.json", Bytes(job_description.begin(), job_description.end()));
            disk_.write(path + "/owner.txt", Bytes(owner.begin(), owner.end()));
        } catch (...) {
            disk_.remove_tree(path);
            throw;
        }
        return id;
    }
    throw std::runtime_error("cannot allocate a job ID");
}

void JobStore::save_tile(const std::string& job_id, unsigned tile_id, const Bytes& pixels) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto path = job_path(job_id);
    if (disk_.exists(path + "/status.json")) throw std::runtime_error("completed job is immutable");
    disk_.write(path + "/tiles/" + std::to_string(tile_id) + ".rgb", pixels);
}

void JobStore::finish(const std::string& job_id, const Bytes& image, const std::string& status_json) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto path = job_path(job_id);
    if (disk_.exists(path + "/status.json")) throw std::runtime_error("completed job is immutable");
    disk_.write(path + "/result.ppm", image);
    // Only the last successful write makes the completed image downloadable.
    disk_.write(path + "/status.json", Bytes(status_json.begin(), status_json.end()));
}

void JobStore::abort(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto path = job_path(job_id);
    if (disk_.exists(path + "/status.json")) return;
    disk_.remove_tree(path);
}

Bytes JobStore::status(const std::string& job_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return disk_.read(job_path(job_id) + "/status.json");
}

Bytes JobStore::download(const std::string& job_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto path = job_path(job_id);
    if (!disk_.exists(path + "/status.json")) throw std::runtime_error("job is unknown or incomplete");
    return disk_.read(path + "/result.ppm");
}

Bytes JobStore::tile(const std::string& job_id, unsigned tile_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto path = job_path(job_id);
    const auto source = decode_ppm(disk_.read(path + "/source.ppm"));
    const auto tiles = partition(source);
    if (tile_id >= tiles.size()) throw std::runtime_error("unknown tile ID");
    const auto& tile = tiles[tile_id];
    auto pixels = disk_.read(path + "/tiles/" + std::to_string(tile_id) + ".rgb");
    if (pixels.size() != static_cast<std::size_t>(tile.width) * tile.height * 3) {
        throw std::runtime_error("stored tile has an invalid pixel length");
    }
    return encode_ppm(Image{tile.width, tile.height, std::move(pixels)});
}

std::string JobStore::owner(const std::string& job_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto bytes = disk_.read(job_path(job_id) + "/owner.txt");
    const std::string value(bytes.begin(), bytes.end());
    if (value != "alice" && value != "bob" && value != "admin") throw std::runtime_error("invalid stored job owner");
    return value;
}
