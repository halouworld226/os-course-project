#pragma once

#include "image.hpp"

#include <array>
#include <stdexcept>

template<class Read>
bool read_exact(Read& read, std::uint8_t* bytes, std::size_t count) {
    for (std::size_t done = 0; done < count;) {
        const auto chunk = read(bytes + done, count - done);
        if (chunk == 0) {
            if (done == 0) return false;
            throw std::runtime_error("truncated stream");
        }
        done += chunk;
    }
    return true;
}

template<class Read>
bool read_frame(Read read, Bytes& payload, std::size_t limit = max_frame) {
    std::array<std::uint8_t, 4> header{};
    if (!read_exact(read, header.data(), header.size())) return false;
    std::uint32_t size = 0;
    for (const auto byte : header) size = (size << 8) | byte;
    if (size > limit) throw std::runtime_error("frame exceeds size limit");
    payload.resize(size);
    if (!read_exact(read, payload.data(), size)) throw std::runtime_error("missing frame body");
    return true;
}

template<class Write>
void write_exact(Write& write, const std::uint8_t* bytes, std::size_t count) {
    for (std::size_t done = 0; done < count;) {
        const auto chunk = write(bytes + done, count - done);
        if (chunk == 0) throw std::runtime_error("stream closed during write");
        done += chunk;
    }
}

template<class Write>
void write_frame(Write write, const Bytes& payload) {
    if (payload.size() > max_frame) throw std::runtime_error("frame exceeds size limit");
    const auto size = static_cast<std::uint32_t>(payload.size());
    const std::array<std::uint8_t, 4> header{static_cast<std::uint8_t>(size >> 24),
        static_cast<std::uint8_t>(size >> 16), static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size)};
    write_exact(write, header.data(), header.size());
    write_exact(write, payload.data(), payload.size());
}
