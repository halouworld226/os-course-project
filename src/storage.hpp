#pragma once

#include "image.hpp"
#include "inode_fs.hpp"

#include <filesystem>
#include <string>
#include <mutex>

class JobStore {
public:
    explicit JobStore(std::filesystem::path root);

    std::string create_job(const Bytes& source, const std::string& job_description,
                           const std::string& owner = "admin");
    void save_tile(const std::string& job_id, unsigned tile_id, const Bytes& pixels);
    void finish(const std::string& job_id, const Bytes& image, const std::string& status_json);
    void abort(const std::string& job_id);
    Bytes status(const std::string& job_id) const;
    Bytes download(const std::string& job_id) const;
    Bytes tile(const std::string& job_id, unsigned tile_id) const;
    std::string owner(const std::string& job_id) const;

private:
    std::string job_path(const std::string& job_id) const;
    mutable std::mutex mutex_;
    InodeFs disk_;
};
