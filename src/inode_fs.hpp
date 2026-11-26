#pragma once

#include "image.hpp"

#include <filesystem>
#include <memory>
#include <string>

// A fixed-size image with a superblock, redundant metadata banks, an inode table,
// a free-block bitmap, and linked data blocks. All paths are absolute.
class InodeFs {
public:
    struct Statistics {
        std::uint64_t capacity;
        unsigned used_inodes;
        unsigned free_blocks;
        unsigned data_blocks;
    };

    explicit InodeFs(const std::filesystem::path& image,
                     std::uint64_t capacity = 128ULL * 1024 * 1024);
    ~InodeFs();
    InodeFs(const InodeFs&) = delete;
    InodeFs& operator=(const InodeFs&) = delete;

    void mkdir(const std::string& path);
    bool exists(const std::string& path) const;
    void write(const std::string& path, const Bytes& data);
    Bytes read(const std::string& path) const;
    void remove(const std::string& path);
    void remove_tree(const std::string& path);
    Statistics statistics() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};
