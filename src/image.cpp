#include "image.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::string_view token(const Bytes& data, std::size_t& pos) {
    while (pos < data.size()) {
        if (std::isspace(data[pos])) {
            ++pos;
        } else if (data[pos] == '#') {
            while (pos < data.size() && data[pos] != '\n') ++pos;
        } else {
            break;
        }
    }
    const auto start = pos;
    while (pos < data.size() && !std::isspace(data[pos]) && data[pos] != '#') ++pos;
    if (start == pos) throw std::runtime_error("incomplete PPM header");
    return {reinterpret_cast<const char*>(data.data() + start), pos - start};
}

unsigned number(std::string_view value) {
    unsigned result = 0;
    const auto end = value.data() + value.size();
    const auto scan = std::from_chars(value.data(), end, result);
    if (scan.ec != std::errc{} || scan.ptr != end) throw std::runtime_error("invalid PPM number");
    return result;
}

std::size_t stride(unsigned width) { return static_cast<std::size_t>(width) * 3; }

} // namespace

Image decode_ppm(const Bytes& data) {
    std::size_t pos = 0;
    if (token(data, pos) != "P6") throw std::runtime_error("upload a binary P6 PPM image");
    const auto width = number(token(data, pos));
    const auto height = number(token(data, pos));
    if (width == 0 || height == 0 || width > max_image_edge || height > max_image_edge) {
        throw std::runtime_error("PPM dimensions must be in 1..2048");
    }
    if (number(token(data, pos)) != 255 || pos == data.size() || !std::isspace(data[pos])) {
        throw std::runtime_error("PPM must have maximum channel value 255 and a raster separator");
    }
    ++pos;
    const auto expected_size = stride(width) * height;
    if (data.size() - pos == expected_size + 1 && data[pos - 1] == '\r' && data[pos] == '\n') ++pos;
    if (data.size() - pos != expected_size) throw std::runtime_error("PPM raster length does not match dimensions");
    return {width, height, Bytes(data.begin() + pos, data.end())};
}

Bytes encode_ppm(const Image& image) {
    const auto header = "P6\n" + std::to_string(image.width) + ' ' + std::to_string(image.height) + "\n255\n";
    Bytes output(header.begin(), header.end());
    output.insert(output.end(), image.pixels.begin(), image.pixels.end());
    return output;
}

std::vector<Tile> partition(const Image& image) {
    std::vector<Tile> tiles;
    for (unsigned y = 0; y < image.height; y += tile_edge) {
        for (unsigned x = 0; x < image.width; x += tile_edge) {
            tiles.push_back({static_cast<unsigned>(tiles.size()), x, y,
                             std::min(tile_edge, image.width - x), std::min(tile_edge, image.height - y)});
        }
    }
    return tiles;
}

Bytes extract_tile(const Image& image, const Tile& tile) {
    Bytes bytes(stride(tile.width) * tile.height);
    for (unsigned row = 0; row < tile.height; ++row) {
        const auto offset = stride(image.width) * (tile.y + row) + stride(tile.x);
        std::copy_n(image.pixels.begin() + offset, stride(tile.width), bytes.begin() + stride(tile.width) * row);
    }
    return bytes;
}

void place_tile(Image& image, const Tile& tile, const Bytes& pixels) {
    if (pixels.size() != stride(tile.width) * tile.height) throw std::runtime_error("invalid worker tile length");
    for (unsigned row = 0; row < tile.height; ++row) {
        const auto offset = stride(image.width) * (tile.y + row) + stride(tile.x);
        std::copy_n(pixels.begin() + stride(tile.width) * row, stride(tile.width), image.pixels.begin() + offset);
    }
}

void process_pixels(Operation operation, Bytes& pixels) {
    if (pixels.size() % 3 != 0) throw std::runtime_error("invalid RGB payload");
    if (operation == Operation::invert) {
        for (auto& value : pixels) value = static_cast<std::uint8_t>(255 - value);
        return;
    }
    if (operation != Operation::grayscale) throw std::runtime_error("unknown image operation");
    for (std::size_t i = 0; i < pixels.size(); i += 3) {
        const auto gray = (299 * pixels[i] + 587 * pixels[i + 1] + 114 * pixels[i + 2] + 500) / 1000;
        pixels[i] = pixels[i + 1] = pixels[i + 2] = static_cast<std::uint8_t>(gray);
    }
}
