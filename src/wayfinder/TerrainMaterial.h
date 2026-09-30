#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace wayfinder {
constexpr std::uint32_t rgba(unsigned r, unsigned g, unsigned b) { return r | (g << 8) | (b << 16) | 0xff000000u; }
inline std::uint32_t tintTopTexture(std::uint32_t texture, std::uint32_t tint) {
    auto channel = [&](int shift) { return (((texture >> shift) & 255u) * ((tint >> shift) & 255u) + 127u) / 255u; };
    return rgba(channel(0), channel(8), channel(16));
}

// Immutable, engine-independent texture snapshots shared by cells and I/O jobs.
// Only the 16x16 source is persisted; alpha-weighted mip levels are rebuilt.
class TerrainMaterial {
public:
    using Pixels = std::array<std::uint32_t, 256>;
    explicit TerrainMaterial(Pixels const& pixels) {
        std::copy(pixels.begin(), pixels.end(), mPixels.begin());
        mHash = 14695981039346656037ull;
        for (auto color : pixels) for (int shift = 0; shift < 32; shift += 8) {
            mHash ^= (color >> shift) & 255u;
            mHash *= 1099511628211ull;
        }
        for (int level = 1; level <= 4; ++level) {
            int side = 16 >> level, previous = side * 2;
            for (int z = 0; z < side; ++z) for (int x = 0; x < side; ++x) {
                unsigned alpha = 0, r = 0, g = 0, b = 0;
                for (int dz = 0; dz < 2; ++dz) for (int dx = 0; dx < 2; ++dx) {
                    auto c = mPixels[offsets[level - 1] + (z * 2 + dz) * previous + x * 2 + dx];
                    auto a = c >> 24;
                    alpha += a; r += (c & 255u) * a; g += ((c >> 8) & 255u) * a; b += ((c >> 16) & 255u) * a;
                }
                mPixels[offsets[level] + z * side + x] = alpha
                    ? ((r + alpha / 2) / alpha) | (((g + alpha / 2) / alpha) << 8)
                        | (((b + alpha / 2) / alpha) << 16) | (((alpha + 2) / 4) << 24) : 0;
            }
        }
    }
    std::span<std::uint32_t const, 256> pixels() const { return std::span(mPixels).first<256>(); }
    std::uint64_t hash() const { return mHash; }
    std::uint32_t average() const { return mPixels.back() | 0xff000000u; }
    bool visible() const { return (mPixels.back() >> 24) != 0; }
    bool operator==(TerrainMaterial const& other) const { return mHash == other.mHash && mPixels == other.mPixels; }
    std::uint32_t texel(int level, double u, double v, unsigned rotation) const {
        // Rotation is clockwise in world X/Z; mirrors are baked from source UVs.
        for (unsigned turn = 0; turn < (rotation & 3u); ++turn) {
            double oldU = u; u = v; v = 1.0 - oldU;
        }
        int side = 16 >> level;
        int x = std::clamp(int(u * side), 0, side - 1), z = std::clamp(int(v * side), 0, side - 1);
        return mPixels[offsets[level] + z * side + x];
    }
private:
    static constexpr std::array<int, 5> offsets{0, 256, 320, 336, 340};
    std::array<std::uint32_t, 341> mPixels{};
    std::uint64_t mHash{};
};
using TerrainMaterialPtr = std::shared_ptr<TerrainMaterial const>;
inline bool sameMaterial(TerrainMaterialPtr const& a, TerrainMaterialPtr const& b) {
    return a == b || (a && b && *a == *b);
}

// Weak interning keeps identical materials shared across resident history tiles,
// without retaining textures after their last tile/snapshot has been released.
class TerrainMaterialPool {
public:
    TerrainMaterialPtr intern(TerrainMaterialPtr material) {
        if (!material) return {};
        if (mEntries.size() >= 4096) {
            std::erase_if(mEntries, [](auto const& entry) { return entry.second.expired(); });
            if (mEntries.size() >= 4096) mEntries.clear();
        }
        auto [first, last] = mEntries.equal_range(material->hash());
        for (auto it = first; it != last; ++it)
            if (auto old = it->second.lock(); old && *old == *material) return old;
        mEntries.emplace(material->hash(), material);
        return material;
    }
private:
    std::unordered_multimap<std::uint64_t, std::weak_ptr<TerrainMaterial const>> mEntries;
};

// Composite alpha over the cell's representative colour, never over the HUD.
// Full scene transparency/geometry is deliberately not inferred from a top UV.
inline std::uint32_t materialPixel(std::uint32_t texel, std::uint32_t tint, std::uint32_t background) {
    auto tinted = tintTopTexture(texel, tint);
    unsigned alpha = texel >> 24;
    auto channel = [&](int shift) {
        return ((((tinted >> shift) & 255u) * alpha + ((background >> shift) & 255u) * (255 - alpha)) + 127) / 255;
    };
    return rgba(channel(0), channel(8), channel(16));
}

inline TerrainMaterialPtr sampleTopMaterial(
    std::span<std::uint8_t const> bytes, int width, int height, int channels, bool bgra,
    float u0, float v0, float u1, float v1
) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || (channels != 3 && channels != 4)
        || bytes.size() < std::size_t(width) * height * channels
        || !std::isfinite(u0) || !std::isfinite(v0) || !std::isfinite(u1) || !std::isfinite(v1)
        || std::min(u0, u1) < 0 || std::max(u0, u1) > 1 || std::min(v0, v1) < 0 || std::max(v0, v1) > 1
        || u0 == u1 || v0 == v1) return {};
    TerrainMaterial::Pixels pixels{};
    int samplesX = std::clamp(int(std::ceil(std::abs(u1 - u0) * width / 16)), 1, 4);
    int samplesZ = std::clamp(int(std::ceil(std::abs(v1 - v0) * height / 16)), 1, 4);
    for (int z = 0; z < 16; ++z) for (int x = 0; x < 16; ++x) {
        unsigned alpha = 0, r = 0, g = 0, b = 0;
        for (int sz = 0; sz < samplesZ; ++sz) for (int sx = 0; sx < samplesX; ++sx) {
            int px = std::clamp(int((u0 + (u1 - u0) * (x + (sx + 0.5) / samplesX) / 16) * width), 0, width - 1);
            int pz = std::clamp(int((v0 + (v1 - v0) * (z + (sz + 0.5) / samplesZ) / 16) * height), 0, height - 1);
            auto index = (std::size_t(pz) * width + px) * channels;
            unsigned a = channels == 3 ? 255 : bytes[index + 3];
            alpha += a; r += bytes[index + (bgra ? 2 : 0)] * a;
            g += bytes[index + 1] * a; b += bytes[index + (bgra ? 0 : 2)] * a;
        }
        unsigned count = unsigned(samplesX * samplesZ);
        if (alpha) pixels[z * 16 + x] = ((r + alpha / 2) / alpha) | (((g + alpha / 2) / alpha) << 8)
            | (((b + alpha / 2) / alpha) << 16) | (((alpha + count / 2) / count) << 24);
    }
    auto material = std::make_shared<TerrainMaterial const>(pixels);
    return material->visible() ? material : TerrainMaterialPtr{};
}

inline std::array<float, 4> sourceTextureUv(float u0, float v0, float u1, float v1,
                                          int atlasWidth, int atlasHeight, int sourceWidth, int sourceHeight) {
    if (atlasWidth <= 0 || atlasHeight <= 0 || sourceWidth <= 0 || sourceHeight <= 0
        || !std::isfinite(u0) || !std::isfinite(v0) || !std::isfinite(u1) || !std::isfinite(v1)
        || std::min(u0, u1) < 0 || std::max(u0, u1) > 1 || std::min(v0, v1) < 0 || std::max(v0, v1) > 1)
        return {u0, v0, u1, v1};
    float width = std::min(1.0f, std::abs(u1 - u0) * atlasWidth / sourceWidth);
    float height = std::min(1.0f, std::abs(v1 - v0) * atlasHeight / sourceHeight);
    // Animated strips use the first frame. Preserve any source UV mirroring.
    return {u1 < u0 ? width : 0.0f, v1 < v0 ? height : 0.0f, u1 < u0 ? 0.0f : width, v1 < v0 ? 0.0f : height};
}
} // namespace wayfinder
