#include "inode_fs.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace {

constexpr unsigned block_size = 4096;
constexpr unsigned payload_size = block_size - 4;
constexpr unsigned inode_count = 8192;
constexpr unsigned inode_size = 96;
constexpr unsigned no_block = std::numeric_limits<unsigned>::max();
constexpr unsigned super_size = 4096;
constexpr unsigned format_version = 1;

void put(Bytes& data, std::size_t offset, std::uint64_t value, unsigned width = 4) {
    for (unsigned i = 0; i < width; ++i) data.at(offset + i) = static_cast<std::uint8_t>(value >> (8 * i));
}

std::uint64_t get(const Bytes& data, std::size_t offset, unsigned width = 4) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= std::uint64_t(data.at(offset + i)) << (8 * i);
    return value;
}

std::uint32_t checksum(const Bytes& bytes) {
    std::uint32_t hash = 2166136261u;
    for (const auto byte : bytes) { hash ^= byte; hash *= 16777619u; }
    return hash;
}

struct Inode {
    unsigned kind = 0; // 0 free, 1 regular file, 2 directory
    unsigned parent = 0;
    unsigned first = no_block;
    std::uint64_t size = 0;
    std::string name;
};

struct Metadata {
    std::uint64_t generation = 0;
    std::vector<Inode> inodes = std::vector<Inode>(inode_count);
    Bytes bitmap;
};

bool allocated(const Bytes& bitmap, unsigned block) { return (bitmap.at(block / 8) & (1u << (block % 8))) != 0; }

void mark(Bytes& bitmap, unsigned block, bool used) {
    auto& byte = bitmap.at(block / 8);
    const auto mask = static_cast<std::uint8_t>(1u << (block % 8));
    byte = used ? static_cast<std::uint8_t>(byte | mask) : static_cast<std::uint8_t>(byte & ~mask);
}

std::vector<std::string> components(const std::string& path) {
    if (path.empty() || path.front() != '/') throw std::runtime_error("filesystem paths must be absolute");
    std::vector<std::string> result;
    for (std::size_t begin = 1; begin < path.size();) {
        const auto end = path.find('/', begin);
        const auto part = path.substr(begin, end == std::string::npos ? end : end - begin);
        if (part.empty() || part == "." || part == ".." || part.size() > 63 || part.find('\0') != std::string::npos) {
            throw std::runtime_error("invalid filesystem path component");
        }
        result.push_back(part);
        if (end == std::string::npos) break;
        begin = end + 1;
        if (begin == path.size()) throw std::runtime_error("trailing slash is not supported");
    }
    return result;
}

unsigned child(const Metadata& meta, unsigned parent, const std::string& name) {
    for (unsigned i = 1; i < meta.inodes.size(); ++i) {
        const auto& node = meta.inodes[i];
        if (node.kind && node.parent == parent && node.name == name) return i;
    }
    return no_block;
}

unsigned resolve(const Metadata& meta, const std::vector<std::string>& parts) {
    unsigned current = 0;
    for (const auto& name : parts) {
        if (meta.inodes[current].kind != 2) throw std::runtime_error("path parent is not a directory");
        current = child(meta, current, name);
        if (current == no_block) return no_block;
    }
    return current;
}

std::pair<unsigned, std::string> parent_of(const Metadata& meta, const std::string& path) {
    auto parts = components(path);
    if (parts.empty()) throw std::runtime_error("cannot replace or delete the root directory");
    auto name = std::move(parts.back());
    parts.pop_back();
    const auto parent = resolve(meta, parts);
    if (parent == no_block || meta.inodes[parent].kind != 2) throw std::runtime_error("parent directory does not exist");
    return {parent, name};
}

unsigned free_inode(const Metadata& meta) {
    for (unsigned i = 1; i < meta.inodes.size(); ++i) if (!meta.inodes[i].kind) return i;
    throw std::runtime_error("inode table is full");
}

} // namespace

struct InodeFs::State {
    mutable std::mutex mutex;
    mutable std::FILE* disk = nullptr;
    std::uint64_t capacity = 0;
    unsigned blocks = 0;
    unsigned bank_size = 0;
    unsigned data_offset = 0;
    unsigned active_bank = 0;
    Metadata meta;
#ifdef _WIN32
    HANDLE lock = INVALID_HANDLE_VALUE;
#else
    int lock = -1;
#endif

    ~State() {
        if (disk) std::fclose(disk);
#ifdef _WIN32
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
#else
        if (lock != -1) close(lock);
#endif
    }

    void read_at(std::uint64_t offset, Bytes& data) const {
#ifdef _WIN32
        const auto positioned = _fseeki64(disk, static_cast<__int64>(offset), SEEK_SET);
#else
        const auto positioned = fseeko(disk, static_cast<off_t>(offset), SEEK_SET);
#endif
        if (positioned != 0 || std::fread(data.data(), 1, data.size(), disk) != data.size()) {
            throw std::runtime_error("disk image read failed");
        }
    }

    void write_at(std::uint64_t offset, const Bytes& data) {
#ifdef _WIN32
        const auto positioned = _fseeki64(disk, static_cast<__int64>(offset), SEEK_SET);
#else
        const auto positioned = fseeko(disk, static_cast<off_t>(offset), SEEK_SET);
#endif
        if (positioned != 0 || std::fwrite(data.data(), 1, data.size(), disk) != data.size()) {
            throw std::runtime_error("disk image write failed");
        }
    }

    void flush() {
        if (std::fflush(disk) != 0) throw std::runtime_error("disk image flush failed");
    }

    Bytes encode(const Metadata& value) const {
        Bytes bytes(bank_size);
        put(bytes, 0, value.generation, 8);
        std::copy(value.bitmap.begin(), value.bitmap.end(), bytes.begin() + 16);
        const auto inode_offset = 16 + value.bitmap.size();
        for (unsigned i = 0; i < inode_count; ++i) {
            const auto& node = value.inodes[i];
            const auto offset = inode_offset + i * inode_size;
            put(bytes, offset, node.kind);
            put(bytes, offset + 4, node.parent);
            put(bytes, offset + 8, node.first);
            put(bytes, offset + 16, node.size, 8);
            std::copy(node.name.begin(), node.name.end(), bytes.begin() + offset + 24);
        }
        put(bytes, 8, checksum(bytes));
        return bytes;
    }

    bool decode(Bytes bytes, Metadata& value) const {
        const auto saved = get(bytes, 8);
        put(bytes, 8, 0);
        if (saved != checksum(bytes) || !get(bytes, 0, 8)) return false;
        value.generation = get(bytes, 0, 8);
        value.bitmap.assign(bytes.begin() + 16, bytes.begin() + 16 + (blocks + 7) / 8);
        const auto inode_offset = 16 + value.bitmap.size();
        for (unsigned i = 0; i < inode_count; ++i) {
            auto& node = value.inodes[i];
            const auto offset = inode_offset + i * inode_size;
            node.kind = static_cast<unsigned>(get(bytes, offset));
            node.parent = static_cast<unsigned>(get(bytes, offset + 4));
            node.first = static_cast<unsigned>(get(bytes, offset + 8));
            node.size = get(bytes, offset + 16, 8);
            const auto start = bytes.begin() + offset + 24;
            const auto end = std::find(start, start + 64, 0);
            if (end == start + 64 || node.kind > 2 || node.parent >= inode_count ||
                (node.first != no_block && node.first >= blocks) || node.size > max_frame) return false;
            node.name.assign(start, end);
        }
        return value.inodes[0].kind == 2;
    }

    void commit(Metadata next) {
        next.generation = meta.generation + 1;
        const auto bank = 1 - active_bank;
        write_at(super_size + std::uint64_t(bank) * bank_size, encode(next));
        flush();
        meta = std::move(next);
        active_bank = bank;
    }

    std::vector<unsigned> chain(const Inode& node) const {
        std::vector<unsigned> result;
        auto block = node.first;
        const auto count = (node.size + payload_size - 1) / payload_size;
        for (std::uint64_t i = 0; i < count; ++i) {
            if (block >= blocks || !allocated(meta.bitmap, block) ||
                std::find(result.begin(), result.end(), block) != result.end()) throw std::runtime_error("corrupt inode block chain");
            result.push_back(block);
            Bytes link(4);
            read_at(data_offset + std::uint64_t(block) * block_size, link);
            block = static_cast<unsigned>(get(link, 0));
        }
        if (block != no_block) throw std::runtime_error("inode block chain length mismatch");
        return result;
    }
};

InodeFs::InodeFs(const std::filesystem::path& image, std::uint64_t requested_capacity) : state_(std::make_unique<State>()) {
    auto& state = *state_;
    if (!image.parent_path().empty()) std::filesystem::create_directories(image.parent_path());
    auto lock_path = image;
    lock_path += ".lock";
#ifdef _WIN32
    state.lock = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (state.lock == INVALID_HANDLE_VALUE) throw std::runtime_error("disk image is already in use or inaccessible");
#else
    state.lock = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (state.lock == -1 || flock(state.lock, LOCK_EX | LOCK_NB) != 0) throw std::runtime_error("disk image is already in use or inaccessible");
    if (fcntl(state.lock, F_SETFD, FD_CLOEXEC) == -1) throw std::runtime_error("cannot secure disk image lock descriptor");
#endif
    const bool fresh = !std::filesystem::exists(image);
    if (fresh) {
        if (requested_capacity < 4ULL * 1024 * 1024 || requested_capacity > 1024ULL * 1024 * 1024) {
            throw std::runtime_error("disk capacity must be between 4 MiB and 1 GiB");
        }
        state.capacity = requested_capacity;
        state.blocks = static_cast<unsigned>((requested_capacity - super_size - 2 * (inode_count * inode_size + 65536)) / block_size);
        state.bank_size = static_cast<unsigned>(((16 + (state.blocks + 7) / 8 + inode_count * inode_size + block_size - 1) / block_size) * block_size);
        state.data_offset = super_size + 2 * state.bank_size;
        std::ofstream create(image, std::ios::binary | std::ios::trunc);
        create.seekp(static_cast<std::streamoff>(requested_capacity - 1));
        create.put(0);
        create.close();
        if (!create) throw std::runtime_error("cannot create fixed-size disk image");
    }
#ifdef _WIN32
    const auto descriptor = _wopen(image.c_str(), _O_BINARY | _O_RDWR | _O_NOINHERIT);
    if (descriptor == -1) throw std::runtime_error("cannot open disk image");
    state.disk = _fdopen(descriptor, "r+b");
    if (!state.disk) _close(descriptor);
#else
    const auto descriptor = open(image.c_str(), O_RDWR | O_CLOEXEC);
    if (descriptor == -1) throw std::runtime_error("cannot open disk image");
    state.disk = fdopen(descriptor, "r+b");
    if (!state.disk) close(descriptor);
#endif
    if (!state.disk) throw std::runtime_error("cannot open disk image");
    if (fresh) {
        Bytes super(super_size);
        const std::string magic = "OSINODE1";
        std::copy(magic.begin(), magic.end(), super.begin());
        put(super, 8, format_version);
        put(super, 12, block_size);
        put(super, 16, state.capacity, 8);
        put(super, 24, state.blocks);
        put(super, 28, state.bank_size);
        put(super, 32, inode_count);
        put(super, 36, state.data_offset);
        put(super, 40, checksum(super));
        state.write_at(0, super);
        state.meta.bitmap.resize((state.blocks + 7) / 8);
        state.meta.inodes[0].kind = 2;
        state.commit(state.meta);
        return;
    }
    Bytes super(super_size);
    state.read_at(0, super);
    const auto saved = get(super, 40);
    put(super, 40, 0);
    if (std::memcmp(super.data(), "OSINODE1", 8) != 0 || get(super, 8) != format_version ||
        get(super, 12) != block_size || get(super, 32) != inode_count || checksum(super) != saved) {
        throw std::runtime_error("invalid or unsupported disk superblock");
    }
    state.capacity = get(super, 16, 8);
    state.blocks = static_cast<unsigned>(get(super, 24));
    state.bank_size = static_cast<unsigned>(get(super, 28));
    state.data_offset = static_cast<unsigned>(get(super, 36));
    const auto minimum_bank = 16ULL + (state.blocks + 7ULL) / 8 + inode_count * inode_size;
    if (!state.blocks || state.capacity != std::filesystem::file_size(image) || state.bank_size < minimum_bank ||
        state.bank_size > 2 * 1024 * 1024 || state.data_offset != super_size + 2 * state.bank_size ||
        state.data_offset + std::uint64_t(state.blocks) * block_size > state.capacity) throw std::runtime_error("invalid disk image geometry");
    bool found = false;
    for (unsigned bank = 0; bank < 2; ++bank) {
        Bytes bytes(state.bank_size);
        state.read_at(super_size + std::uint64_t(bank) * state.bank_size, bytes);
        Metadata candidate;
        if (state.decode(std::move(bytes), candidate) && (!found || candidate.generation > state.meta.generation)) {
            state.meta = std::move(candidate);
            state.active_bank = bank;
            found = true;
        }
    }
    if (!found) throw std::runtime_error("both inode metadata banks are invalid");
}

InodeFs::~InodeFs() = default;

bool InodeFs::exists(const std::string& path) const {
    auto& state = *state_;
    std::lock_guard<std::mutex> lock(state.mutex);
    return resolve(state.meta, components(path)) != no_block;
}

void InodeFs::mkdir(const std::string& path) {
    auto& state = *state_;
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto [parent, name] = parent_of(state.meta, path);
    const auto existing = child(state.meta, parent, name);
    if (existing != no_block) {
        if (state.meta.inodes[existing].kind != 2) throw std::runtime_error("directory path names a file");
        return;
    }
    auto next = state.meta;
    next.inodes[free_inode(next)] = {2, parent, no_block, 0, name};
    state.commit(std::move(next));
}

void InodeFs::write(const std::string& path, const Bytes& data) {
    auto& state = *state_;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (data.size() > max_frame) throw std::runtime_error("file exceeds 16 MiB limit");
    const auto [parent, name] = parent_of(state.meta, path);
    auto index = child(state.meta, parent, name);
    if (index != no_block && state.meta.inodes[index].kind != 1) throw std::runtime_error("cannot overwrite a directory");
    if (index == no_block) index = free_inode(state.meta);
    const auto old_blocks = state.chain(state.meta.inodes[index]);
    auto next = state.meta;
    const auto needed = (data.size() + payload_size - 1) / payload_size;
    std::vector<unsigned> selected;
    for (unsigned block = 0; block < state.blocks && selected.size() < needed; ++block) {
        if (!allocated(state.meta.bitmap, block)) selected.push_back(block);
    }
    if (selected.size() != needed) throw std::runtime_error("disk image has insufficient free blocks for atomic write");
    for (std::size_t i = 0; i < selected.size(); ++i) {
        Bytes block(block_size);
        put(block, 0, i + 1 < selected.size() ? selected[i + 1] : no_block);
        const auto offset = i * payload_size;
        std::copy_n(data.begin() + offset, std::min<std::size_t>(payload_size, data.size() - offset), block.begin() + 4);
        state.write_at(state.data_offset + std::uint64_t(selected[i]) * block_size, block);
        mark(next.bitmap, selected[i], true);
    }
    state.flush();
    for (const auto block : old_blocks) mark(next.bitmap, block, false);
    next.inodes[index] = {1, parent, selected.empty() ? no_block : selected.front(), data.size(), name};
    state.commit(std::move(next));
}

Bytes InodeFs::read(const std::string& path) const {
    auto& state = *state_;
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto index = resolve(state.meta, components(path));
    if (index == no_block || state.meta.inodes[index].kind != 1) throw std::runtime_error("file does not exist");
    const auto& node = state.meta.inodes[index];
    const auto blocks = state.chain(node);
    Bytes data;
    data.reserve(static_cast<std::size_t>(node.size));
    for (const auto block : blocks) {
        Bytes bytes(block_size);
        state.read_at(state.data_offset + std::uint64_t(block) * block_size, bytes);
        const auto count = std::min<std::size_t>(payload_size, static_cast<std::size_t>(node.size) - data.size());
        data.insert(data.end(), bytes.begin() + 4, bytes.begin() + 4 + count);
    }
    return data;
}

void InodeFs::remove(const std::string& path) {
    auto& state = *state_;
    std::lock_guard<std::mutex> lock(state.mutex);
    parent_of(state.meta, path);
    const auto index = resolve(state.meta, components(path));
    if (index == no_block) throw std::runtime_error("path does not exist");
    const auto& node = state.meta.inodes[index];
    if (node.kind == 2 && std::any_of(state.meta.inodes.begin() + 1, state.meta.inodes.end(), [&](const auto& candidate) {
        return candidate.kind && candidate.parent == index;
    })) throw std::runtime_error("directory is not empty");
    const auto blocks = state.chain(node);
    auto next = state.meta;
    for (const auto block : blocks) mark(next.bitmap, block, false);
    next.inodes[index] = Inode{};
    state.commit(std::move(next));
}

void InodeFs::remove_tree(const std::string& path) {
    auto& state = *state_;
    std::lock_guard<std::mutex> lock(state.mutex);
    parent_of(state.meta, path);
    const auto index = resolve(state.meta, components(path));
    if (index == no_block) return;
    std::vector<unsigned> descendants{index};
    for (std::size_t cursor = 0; cursor < descendants.size(); ++cursor) {
        if (state.meta.inodes[descendants[cursor]].kind != 2) continue;
        for (unsigned i = 1; i < state.meta.inodes.size(); ++i) {
            const auto& node = state.meta.inodes[i];
            if (node.kind && node.parent == descendants[cursor]) descendants.push_back(i);
        }
        if (descendants.size() > inode_count) throw std::runtime_error("corrupt directory hierarchy");
    }
    auto next = state.meta;
    for (const auto inode : descendants) {
        for (const auto block : state.chain(state.meta.inodes[inode])) mark(next.bitmap, block, false);
        next.inodes[inode] = Inode{};
    }
    state.commit(std::move(next));
}

InodeFs::Statistics InodeFs::statistics() const {
    auto& state = *state_;
    std::lock_guard<std::mutex> lock(state.mutex);
    unsigned used = 0, free = 0;
    for (const auto& node : state.meta.inodes) if (node.kind) ++used;
    for (unsigned i = 0; i < state.blocks; ++i) if (!allocated(state.meta.bitmap, i)) ++free;
    return {state.capacity, used, free, state.blocks};
}
