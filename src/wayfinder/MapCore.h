#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

namespace wayfinder {

// Minecraft coordinates use floor division, including west/north of the origin.
constexpr int floorDiv(int value, int divisor) {
    int quotient = value / divisor;
    return quotient - (value % divisor < 0 ? 1 : 0);
}
constexpr int           localBlock(int value) { return value - floorDiv(value, 16) * 16; }
constexpr std::uint32_t rgba(unsigned r, unsigned g, unsigned b) { return r | (g << 8) | (b << 16) | 0xff000000u; }

struct TileKey {
    int  dimension{};
    int  x{};
    int  z{};
    bool operator==(TileKey const&) const = default;
};
struct TileKeyHash {
    std::size_t operator()(TileKey const& key) const noexcept {
        auto mix = [](std::uint64_t v) {
            v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ull;
            v = (v ^ (v >> 27)) * 0x94d049bb133111ebull;
            return v ^ (v >> 31);
        };
        return static_cast<std::size_t>(
            mix(static_cast<std::uint32_t>(key.x)) ^ mix(static_cast<std::uint32_t>(key.z) + 0x9e3779b97f4a7c15ull)
            ^ mix(static_cast<std::uint32_t>(key.dimension) + 0x517cc1b727220a95ull)
        );
    }
};
struct MapCell {
    std::uint32_t color{}; // Alpha zero means unknown, never air.
    std::int16_t  height{};
    std::uint8_t  depth{}; // Water column depth in blocks; zero for dry land.
    bool          operator==(MapCell const&) const = default;
    bool          known() const { return (color >> 24) != 0; }
};
struct MapTile {
    std::array<MapCell, 256> cells{};
    std::uint64_t            lastTouched{};
};
struct TileRecord {
    TileKey key;
    MapTile tile;
};
struct MapBounds {
    double minX{}, minZ{}, maxX{}, maxZ{};
};
struct MapView {
    double centerX{}, centerZ{};
    double blocksPerPixel{2.0};
    int    width{128}, height{128};

    std::array<double, 2> worldAt(double pixelX, double pixelY) const {
        return {centerX + (pixelX - width * 0.5) * blocksPerPixel, centerZ + (pixelY - height * 0.5) * blocksPerPixel};
    }
    void zoomAt(double factor, double pixelX, double pixelY) {
        auto before     = worldAt(pixelX, pixelY);
        blocksPerPixel  = std::clamp(blocksPerPixel * factor, 0.5, 128.0);
        auto after      = worldAt(pixelX, pixelY);
        centerX        += before[0] - after[0];
        centerZ        += before[1] - after[1];
    }
    bool operator==(MapView const&) const = default;
};

class MapCache {
public:
    explicit MapCache(std::size_t capacity = 8192) : mCapacity(std::max<std::size_t>(1, capacity)) {}
    MapCell get(int dimension, int x, int z) const {
        auto it = mTiles.find({dimension, floorDiv(x, 16), floorDiv(z, 16)});
        if (it == mTiles.end()) return {};
        return it->second.cells[localBlock(z) * 16 + localBlock(x)];
    }
    bool put(int dimension, int x, int z, MapCell cell) {
        if (!cell.known()) return false; // Missing subchunks must not erase explored terrain.
        TileKey key{dimension, floorDiv(x, 16), floorDiv(z, 16)};
        auto    it = mTiles.find(key);
        if (it == mTiles.end()) {
            if (mTiles.size() >= mCapacity) {
                auto oldest = std::min_element(mTiles.begin(), mTiles.end(), [](auto const& a, auto const& b) {
                    return a.second.lastTouched < b.second.lastTouched;
                });
                mTiles.erase(oldest);
            }
            it = mTiles.try_emplace(key).first;
        }
        it->second.lastTouched = ++mTouch;
        auto& previous         = it->second.cells[localBlock(z) * 16 + localBlock(x)];
        if (previous == cell) return false;
        previous = cell;
        ++mRevision;
        return true;
    }
    std::optional<MapBounds> bounds(int dimension) const {
        std::optional<MapBounds> result;
        for (auto const& [key, tile] : mTiles) {
            if (key.dimension != dimension) continue;
            for (int i = 0; i < 256; ++i) {
                if (!tile.cells[i].known()) continue;
                double x = key.x * 16.0 + i % 16;
                double z = key.z * 16.0 + i / 16;
                if (!result) result = MapBounds{x, z, x + 1, z + 1};
                else {
                    result->minX = std::min(result->minX, x);
                    result->minZ = std::min(result->minZ, z);
                    result->maxX = std::max(result->maxX, x + 1);
                    result->maxZ = std::max(result->maxZ, z + 1);
                }
            }
        }
        return result;
    }
    std::vector<TileRecord> snapshot() const {
        std::vector<TileRecord> result;
        result.reserve(mTiles.size());
        for (auto const& [key, tile] : mTiles) result.push_back({key, tile});
        return result;
    }
    void restore(std::vector<TileRecord> records) {
        clear();
        for (auto& record : records) {
            if (mTiles.size() == mCapacity) break;
            record.tile.lastTouched = ++mTouch;
            mTiles.insert_or_assign(record.key, std::move(record.tile));
        }
        ++mRevision;
    }
    void clear() {
        mTiles.clear();
        ++mRevision;
    }
    std::uint64_t revision() const { return mRevision; }
    std::size_t   size() const { return mTiles.size(); }
    std::size_t   capacity() const { return mCapacity; }

    // A CPU image owns only plain values; no Minecraft objects escape into worker jobs.
    std::vector<std::uint32_t> rasterize(int dimension, MapView const& view) const {
        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(view.width) * view.height);
        for (int row = 0; row < view.height; ++row) {
            for (int col = 0; col < view.width; ++col) {
                auto  world = view.worldAt(col + 0.5, row + 0.5);
                auto  x     = static_cast<int>(std::floor(world[0]));
                auto  z     = static_cast<int>(std::floor(world[1]));
                auto  cell  = get(dimension, x, z);
                auto& pixel = pixels[static_cast<std::size_t>(row) * view.width + col];
                if (!cell.known()) {
                    pixel = ((floorDiv(x, 16) ^ floorDiv(z, 16)) & 1) ? rgba(27, 33, 40) : rgba(32, 39, 47);
                    continue;
                }
                // Java-style relief: compare with the northern neighbour in three steps,
                // water is shaded by depth with a light dither instead of by surface height.
                float shade = 1.0f; // Flat ground keeps its true colour.
                if (cell.depth > 0) {
                    double level = cell.depth * 0.1 + ((x + z) & 1) * 0.2;
                    shade        = level < 0.5 ? 1.0f : level > 0.9 ? 0.72f : 0.86f;
                } else if (auto north = get(dimension, x, z - 1); north.known()) {
                    shade = cell.height > north.height ? 1.12f : cell.height < north.height ? 0.8f : 1.0f;
                }
                auto channel = [&](int shift) {
                    return static_cast<unsigned>(std::clamp(((cell.color >> shift) & 255u) * shade, 0.0f, 255.0f));
                };
                pixel = rgba(channel(0), channel(8), channel(16));
            }
        }
        return pixels;
    }

private:
    std::unordered_map<TileKey, MapTile, TileKeyHash> mTiles;
    std::size_t                                       mCapacity;
    std::uint64_t                                     mRevision{}, mTouch{};
};

} // namespace wayfinder
