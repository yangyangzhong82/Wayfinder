#pragma once
#include "wayfinder/MapCore.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>

class Block;
class BlockPos;
struct TextureUVCoordinateSet;
namespace mce { class TextureGroup; }

namespace wayfinder {
// Average only the visible top-face UV rectangle. This stays useful when a
// resource pack changes a material without adding per-texel data to map files.
inline std::optional<std::uint32_t> averageTopTexture(
    std::span<std::uint8_t const> bytes, int width, int height, int channels, bool bgra,
    float u0, float v0, float u1, float v1
) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384
        || (channels != 3 && channels != 4)
        || bytes.size() < std::size_t(width) * height * channels
        || !std::isfinite(u0) || !std::isfinite(v0) || !std::isfinite(u1) || !std::isfinite(v1)
        || u0 < 0 || v0 < 0 || u1 > 1 || v1 > 1 || u1 <= u0 || v1 <= v0) return {};
    double red = 0, green = 0, blue = 0, weight = 0;
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
        int px = std::clamp(int((u0 + (u1 - u0) * (x + 0.5f) / 8.0f) * width), 0, width - 1);
        int py = std::clamp(int((v0 + (v1 - v0) * (y + 0.5f) / 8.0f) * height), 0, height - 1);
        auto index = (std::size_t(py) * width + px) * channels;
        double alpha = channels == 3 ? 1.0 : bytes[index + 3] / 255.0;
        red += bytes[index + (bgra ? 2 : 0)] * alpha;
        green += bytes[index + 1] * alpha;
        blue += bytes[index + (bgra ? 0 : 2)] * alpha;
        weight += alpha;
    }
    if (weight < 8) return {};
    return rgba(static_cast<unsigned>(red / weight + 0.5),
                static_cast<unsigned>(green / weight + 0.5),
                static_cast<unsigned>(blue / weight + 0.5));
}

class TerrainTextureColors {
public:
    struct Top { TerrainMaterialPtr material; std::uint8_t rotation{}; };
    void beginTick(mce::TextureGroup* group);
    void reset();
    Top top(Block const& block, BlockPos const& pos);
private:
    struct Face { std::uint64_t slot{1}; int variant{}; std::uint8_t rotation{}; };
    mce::TextureGroup* mGroup{};
    std::unordered_map<TextureUVCoordinateSet const*, TerrainMaterialPtr> mMaterials;
    std::unordered_map<Block const*, Face> mFaces;
    TerrainMaterialPool mPool;
    int mNewTextures{};
    int mTicks{};
};
} // namespace wayfinder
