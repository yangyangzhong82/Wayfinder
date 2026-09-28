#pragma once

#include "wayfinder/MapLayer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wayfinder {

// Minecraft coordinates use floor division, including west/north of the origin.
constexpr int floorDiv(int value, int divisor) {
    int quotient = value / divisor;
    return quotient - (value % divisor < 0 ? 1 : 0);
}
inline int              blockCoordinate(double value) { return static_cast<int>(std::floor(value)); }
constexpr int           localBlock(int value) { return value - floorDiv(value, 16) * 16; }
constexpr std::uint32_t rgba(unsigned r, unsigned g, unsigned b) { return r | (g << 8) | (b << 16) | 0xff000000u; }

struct TileKey {
    int  dimension{};
    int  x{};
    int  z{};
    int  slice{surfaceSlice};
    MapLayer layer() const { return {dimension, slice}; }
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
            ^ mix(static_cast<std::uint32_t>(key.slice) + 0x6eed0e9da4d94a4full)
        );
    }
};
struct MapCell {
    std::uint32_t color{}; // Alpha zero means unknown, never air.
    std::int16_t  height{};
    std::uint8_t  depth{}; // Water column depth in blocks; zero for dry land.
    std::uint8_t  flags{}; // v4: bit 0 wall, bit 1 explored void; neither is a teleport floor.
    static constexpr std::uint8_t wall = 1, voidSpace = 2;
    bool          operator==(MapCell const&) const = default;
    bool          known() const { return (color >> 24) != 0; }
    bool          floor() const { return known() && flags == 0; }
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
    void pan(double dx, double dz, double seconds) {
        auto length = std::hypot(dx, dz);
        if (length == 0) return;
        double step  = blocksPerPixel * 240.0 * std::clamp(seconds, 0.0, 0.1) / length;
        centerX     += dx * step;
        centerZ     += dz * step;
    }
    bool operator==(MapView const&) const = default;
};

struct ColorAggregate {
    double r{}, g{}, b{}, count{};
    void   add(std::uint32_t color) {
        r += color & 255u;
        g += (color >> 8) & 255u;
        b += (color >> 16) & 255u;
        ++count;
    }
};
inline std::uint32_t shadedColor(MapCell cell, MapCell north, MapCell west) {
    if (!cell.known()) return 0;
    if (cell.flags) return cell.color;
    float shade;
    if (cell.depth > 0) {
        // Continuous depth shading makes shores and deep channels distinguishable.
        shade = 1.08f - 0.50f * cell.depth / (cell.depth + 10.0f);
    } else {
        auto slope = [&](MapCell neighbor) {
            if (!neighbor.floor()) return 0.0f;
            float difference = static_cast<float>(cell.height - neighbor.height);
            return std::copysign(std::log2(1.0f + std::abs(difference)), difference);
        };
        // Northwest lighting reacts to both slope direction and height difference.
        // A gentle altitude tint also separates broad, flat plateaus after zooming out.
        float altitude = std::clamp(1.0f + (cell.height - 64) * 0.0012f, 0.86f, 1.16f);
        shade = std::clamp(1.0f + 0.16f * slope(north) + 0.12f * slope(west), 0.48f, 1.48f) * altitude;
        auto contour = [&](MapCell neighbor) {
            return neighbor.floor() && !neighbor.depth && cell.height > neighbor.height
                && floorDiv(cell.height, 8) != floorDiv(neighbor.height, 8);
        };
        // Draw only the upper side of an 8-block contour, never an exploration boundary.
        if (contour(north) || contour(west)) shade *= 0.82f;
    }
    auto channel = [&](int shift) {
        return static_cast<unsigned>(std::clamp(((cell.color >> shift) & 255u) * shade, 0.0f, 255.0f));
    };
    return rgba(channel(0), channel(8), channel(16));
}
inline ColorAggregate tileOverview(MapTile const& tile) {
    ColorAggregate result;
    for (int i = 0; i < 256; ++i)
        if (tile.cells[i].known())
            result.add(shadedColor(
                tile.cells[i],
                i >= 16 ? tile.cells[i - 16] : MapCell{},
                i % 16 ? tile.cells[i - 1] : MapCell{}
            ));
    return result;
}
// Area filtering. Unknown cells have zero weight, so small explored islands remain visible.
class MapRaster {
public:
    explicit MapRaster(MapView view)
    : mView(view),
      mOrigin(view.worldAt(0, 0)),
      mColors(static_cast<std::size_t>(view.width) * view.height) {}
    bool intersects(TileKey key) const {
        return key.x * 16.0 < mOrigin[0] + mView.width * mView.blocksPerPixel && key.x * 16.0 + 16 > mOrigin[0]
            && key.z * 16.0 < mOrigin[1] + mView.height * mView.blocksPerPixel && key.z * 16.0 + 16 > mOrigin[1];
    }
    void add(double x, double z, int side, ColorAggregate const& color) {
        if (color.count == 0) return;
        double left = (x - mOrigin[0]) / mView.blocksPerPixel, top = (z - mOrigin[1]) / mView.blocksPerPixel;
        double right = left + side / mView.blocksPerPixel, bottom = top + side / mView.blocksPerPixel;
        for (int row = std::max(0, blockCoordinate(top));
             row < std::min(mView.height, static_cast<int>(std::ceil(bottom)));
             ++row)
            for (int col = std::max(0, blockCoordinate(left));
                 col < std::min(mView.width, static_cast<int>(std::ceil(right)));
                 ++col) {
                double weight  = (std::min(right, col + 1.0) - std::max(left, double(col)))
                               * (std::min(bottom, row + 1.0) - std::max(top, double(row))) / (side * side);
                auto&  dest    = mColors[static_cast<std::size_t>(row) * mView.width + col];
                dest.r        += color.r * weight;
                dest.g        += color.g * weight;
                dest.b        += color.b * weight;
                dest.count    += color.count * weight;
            }
    }
    template <class North, class West>
    void tile(TileKey key, MapTile const& tile, North north, West west) {
        if (!intersects(key)) return;
        int step = 1;
        while (step < 16 && step * 2 <= mView.blocksPerPixel / 2) step *= 2;
        for (int z = 0; z < 16; z += step)
            for (int x = 0; x < 16; x += step) {
                ColorAggregate color;
                for (int dz = 0; dz < step; ++dz)
                    for (int dx = 0; dx < step; ++dx) {
                        int  index = (z + dz) * 16 + x + dx;
                        auto cell  = tile.cells[index];
                        if (!cell.known()) continue;
                        auto above = index >= 16 ? tile.cells[index - 16] : north(x + dx);
                        auto left = (x + dx) > 0 ? tile.cells[index - 1] : west(z + dz);
                        color.add(shadedColor(cell, above, left));
                    }
                add(key.x * 16.0 + x, key.z * 16.0 + z, step, color);
            }
    }
    std::vector<std::uint32_t> finish() const {
        std::vector<std::uint32_t> pixels(mColors.size());
        for (int row = 0; row < mView.height; ++row)
            for (int col = 0; col < mView.width; ++col) {
                auto        index = static_cast<std::size_t>(row) * mView.width + col;
                auto const& c     = mColors[index];
                if (c.count > 0)
                    pixels[index] = rgba(
                        static_cast<unsigned>(c.r / c.count + 0.5),
                        static_cast<unsigned>(c.g / c.count + 0.5),
                        static_cast<unsigned>(c.b / c.count + 0.5)
                    );
                else {
                    auto world = mView.worldAt(col + 0.5, row + 0.5);
                    pixels[index] =
                        ((floorDiv(blockCoordinate(world[0]), 16) ^ floorDiv(blockCoordinate(world[1]), 16)) & 1)
                            ? rgba(23, 30, 38)
                            : rgba(26, 33, 41);
                }
            }
        return pixels;
    }

private:
    MapView                     mView;
    std::array<double, 2>       mOrigin;
    std::vector<ColorAggregate> mColors;
};

class MapCache {
public:
    explicit MapCache(std::size_t capacity = 8192) : mCapacity(std::max<std::size_t>(1, capacity)) {}
    MapCell get(MapLayer layer, int x, int z) const {
        auto it = mTiles.find({layer.dimension, floorDiv(x, 16), floorDiv(z, 16), layer.slice});
        if (it == mTiles.end()) return {};
        return it->second.cells[localBlock(z) * 16 + localBlock(x)];
    }
    bool put(MapLayer layer, int x, int z, MapCell cell) {
        if (!cell.known()) return false; // Missing subchunks must not erase explored terrain.
        TileKey key{layer.dimension, floorDiv(x, 16), floorDiv(z, 16), layer.slice};
        auto    it = mTiles.find(key);
        if (it == mTiles.end()) {
            if (mTiles.size() >= mCapacity) {
                auto oldest = std::min_element(mTiles.begin(), mTiles.end(), [](auto const& a, auto const& b) {
                    return a.second.lastTouched < b.second.lastTouched;
                });
                if (mDirty.erase(oldest->first)) mEvicted.push_back({oldest->first, oldest->second});
                mTiles.erase(oldest);
            }
            it = mTiles.try_emplace(key).first;
        }
        it->second.lastTouched = ++mTouch;
        mDirty.insert(key);
        auto& previous = it->second.cells[localBlock(z) * 16 + localBlock(x)];
        if (previous == cell) return false;
        previous = cell;
        ++mRevision;
        return true;
    }
    bool complete(TileKey key) const {
        auto it = mTiles.find(key);
        return it != mTiles.end()
            && std::all_of(it->second.cells.begin(), it->second.cells.end(), [](auto cell) { return cell.known(); });
    }
    std::vector<TileRecord> takeChanges() {
        auto result = std::move(mEvicted);
        mEvicted.clear();
        for (auto key : mDirty)
            if (auto it = mTiles.find(key); it != mTiles.end()) result.push_back({key, it->second});
        mDirty.clear();
        return result;
    }
    std::size_t              pendingEvictions() const { return mEvicted.size(); }
    std::optional<MapBounds> bounds(MapLayer layer) const {
        std::optional<MapBounds> result;
        for (auto const& [key, tile] : mTiles) {
            if (key.layer() != layer) continue;
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
        std::stable_sort(records.begin(), records.end(), [](auto const& a, auto const& b) {
            return a.tile.lastTouched > b.tile.lastTouched;
        });
        for (auto& record : records) {
            if (mTiles.size() == mCapacity) break;
            mTouch = std::max(mTouch, record.tile.lastTouched);
            mTiles.insert_or_assign(record.key, std::move(record.tile));
        }
        ++mRevision;
    }
    // Merge an asynchronous warm start without erasing samples/evictions gathered while loading.
    void mergeHistory(std::vector<TileRecord> const& records) {
        for (auto const& record : records) mTouch = std::max(mTouch, record.tile.lastTouched);
        for (auto& [key, tile] : mTiles) tile.lastTouched = ++mTouch;
        for (auto& record : mEvicted) record.tile.lastTouched = ++mTouch;
        for (auto const& record : records) {
            auto it = mTiles.find(record.key);
            if (it == mTiles.end()) {
                if (mTiles.size() < mCapacity) mTiles.emplace(record.key, record.tile);
            } else {
                for (std::size_t i = 0; i < record.tile.cells.size(); ++i)
                    if (!it->second.cells[i].known()) it->second.cells[i] = record.tile.cells[i];
            }
        }
        ++mRevision;
    }
    void clear() {
        mTiles.clear();
        mDirty.clear();
        mEvicted.clear();
        mTouch = 0;
        ++mRevision;
    }
    std::uint64_t revision() const { return mRevision; }
    std::size_t   size() const { return mTiles.size(); }
    std::size_t   capacity() const { return mCapacity; }

    std::vector<std::uint32_t> rasterize(MapLayer layer, MapView const& view) const {
        MapRaster raster(view);
        for (auto const& [key, tile] : mTiles)
            if (key.layer() == layer)
                raster.tile(key, tile,
                    [&](int x) { return get(layer, key.x * 16 + x, key.z * 16 - 1); },
                    [&](int z) { return get(layer, key.x * 16 - 1, key.z * 16 + z); });
        return raster.finish();
    }

private:
    std::unordered_map<TileKey, MapTile, TileKeyHash> mTiles;
    std::unordered_set<TileKey, TileKeyHash>          mDirty;
    std::vector<TileRecord>                           mEvicted;
    std::size_t                                       mCapacity;
    std::uint64_t                                     mRevision{}, mTouch{};
};

} // namespace wayfinder
