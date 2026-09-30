#pragma once
#include "wayfinder/MapCore.h"

namespace wayfinder {
// Bedrock uses MT19937's first unsigned output % 10, seeded only by chunk
// coordinates. Arithmetic intentionally wraps at 32 bits, including negatives.
inline bool isSlimeChunk(int chunkX, int chunkZ) {
    std::uint32_t seed = (static_cast<std::uint32_t>(chunkX) * 0x1f1f1f1fu)
        ^ static_cast<std::uint32_t>(chunkZ);
    // Only state[0], state[1] and state[397] are needed for the first output.
    // Avoid initializing/twisting the rest of the generator for each map chunk.
    std::uint32_t first = 1812433253u * (seed ^ (seed >> 30)) + 1u;
    std::uint32_t state = first;
    for (std::uint32_t i = 2; i <= 397; ++i) state = 1812433253u * (state ^ (state >> 30)) + i;
    auto joined = (seed & 0x80000000u) | (first & 0x7fffffffu);
    auto value = state ^ (joined >> 1) ^ ((joined & 1u) ? 0x9908b0dfu : 0u);
    value ^= value >> 11;
    value ^= (value << 7) & 0x9d2c5680u;
    value ^= (value << 15) & 0xefc60000u;
    value ^= value >> 18;
    return value % 10 == 0;
}
inline bool slimeOverlayVisible(MapLayer layer, MapView const& texture, double blocksPerGui) {
    if (layer.dimension != 0 || blocksPerGui <= 0 || 16 / blocksPerGui < 6
        || texture.width <= 0 || texture.height <= 0 || texture.blocksPerPixel <= 0) return false;
    // Bound work even on unusually large windows. Zooming in reveals the overlay.
    return (std::ceil(texture.width * texture.blocksPerPixel / 16) + 1)
        * (std::ceil(texture.height * texture.blocksPerPixel / 16) + 1) <= 16384;
}
inline void overlaySlimeChunks(std::vector<std::uint32_t>& pixels, MapView const& view, MapLayer layer) {
    if (!slimeOverlayVisible(layer, view, 1.0)
        || pixels.size() != static_cast<std::size_t>(view.width) * view.height) return;
    auto origin = view.worldAt(0, 0);
    auto end = view.worldAt(view.width, view.height);
    // World coordinates beyond the supported map bounds have no chunk overlay.
    int minX = blockCoordinate(std::max(-29999984.0, origin[0]) / 16);
    int minZ = blockCoordinate(std::max(-29999984.0, origin[1]) / 16);
    int maxX = static_cast<int>(std::ceil(std::min(29999984.0, end[0]) / 16));
    int maxZ = static_cast<int>(std::ceil(std::min(29999984.0, end[1]) / 16));
    auto edge = [&](double world, double start, int extent) {
        return static_cast<int>(std::clamp(std::ceil((world - start) / view.blocksPerPixel - 0.5), 0.0, double(extent)));
    };
    for (int z = minZ; z < maxZ; ++z) for (int x = minX; x < maxX; ++x) {
        if (!isSlimeChunk(x, z)) continue;
        int left = edge(x * 16.0, origin[0], view.width), right = edge((x + 1) * 16.0, origin[0], view.width);
        int top = edge(z * 16.0, origin[1], view.height), bottom = edge((z + 1) * 16.0, origin[1], view.height);
        for (int py = top; py < bottom; ++py) for (int px = left; px < right; ++px) {
            auto& pixel = pixels[static_cast<std::size_t>(py) * view.width + px];
            // A translucent green fill keeps terrain, fog and height shading legible.
            pixel = rgba(((pixel & 255u) * 3 + 80) / 4,
                         (((pixel >> 8) & 255u) * 3 + 255) / 4,
                         (((pixel >> 16) & 255u) * 3 + 95) / 4);
        }
    }
}
} // namespace wayfinder
