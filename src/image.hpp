#pragma once

#include <cstdint>
#include <vector>

using Bytes = std::vector<std::uint8_t>;
enum class Operation : std::uint8_t { grayscale = 1, invert = 2 };

constexpr unsigned tile_edge = 64;
constexpr unsigned max_image_edge = 2048;
constexpr std::size_t max_frame = 16 * 1024 * 1024;

struct Image {
    unsigned width;
    unsigned height;
    Bytes pixels;
};

struct Tile {
    unsigned id;
    unsigned x;
    unsigned y;
    unsigned width;
    unsigned height;
};

Image decode_ppm(const Bytes& data);
Bytes encode_ppm(const Image& image);
std::vector<Tile> partition(const Image& image);
Bytes extract_tile(const Image& image, const Tile& tile);
void place_tile(Image& image, const Tile& tile, const Bytes& pixels);
void process_pixels(Operation operation, Bytes& pixels);
