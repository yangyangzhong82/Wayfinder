#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace wayfinder {
struct PortraitPixels {
    int width{}, height{};
    std::vector<std::uint32_t> pixels;
};
// Entity materials use low alpha values for tint/emissive masks (for example
// Enderman eyes and sheep faces). UI materials interpret those values as nearly
// transparent. Normalize a private copy, keeping truly transparent texels clear.
inline PortraitPixels portraitPixels(std::span<std::uint8_t const> source, int width, int height,
                                     int channels, bool bgra = false) {
    PortraitPixels result;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384
        || (channels != 3 && channels != 4)
        || source.size() < std::size_t(width) * height * channels) return result;
    double scale = std::min(1.0, 256.0 / std::max(width, height));
    result.width = std::max(1, int(width * scale));
    result.height = std::max(1, int(height * scale));
    result.pixels.resize(std::size_t(result.width) * result.height);
    for (int y = 0; y < result.height; ++y) {
        int sy = std::min(height - 1, int((y + 0.5) * height / result.height));
        for (int x = 0; x < result.width; ++x) {
            int sx = std::min(width - 1, int((x + 0.5) * width / result.width));
            auto i = (std::size_t(sy) * width + sx) * channels;
            auto r = source[i + (bgra ? 2 : 0)], g = source[i + 1], b = source[i + (bgra ? 0 : 2)];
            std::uint32_t alpha = channels == 3 || source[i + 3] != 0 ? 255u : 0u;
            result.pixels[std::size_t(y) * result.width + x] = r | (std::uint32_t(g) << 8)
                | (std::uint32_t(b) << 16) | (alpha << 24);
        }
    }
    return result;
}
} // namespace wayfinder
