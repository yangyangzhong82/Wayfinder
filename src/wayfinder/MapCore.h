#pragma once

#include "wayfinder/MapLayer.h"
#include "wayfinder/BiomeTile.h"
#include "wayfinder/TerrainMaterial.h"

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
    std::uint8_t  skyLight{255}, blockLight{255}; // 0..15; 255 means not sampled (old history).
    static constexpr std::uint8_t wall = 1, voidSpace = 2;
    TerrainMaterialPtr material{};
    std::uint32_t tint{0xffffffffu};
    std::uint8_t rotation{};
    std::uint64_t surfaceId{}; // Serialized block-state hash; zero for legacy/unknown.
    bool operator==(MapCell const& other) const {
        return color == other.color && height == other.height && depth == other.depth && flags == other.flags
            && skyLight == other.skyLight && blockLight == other.blockLight && tint == other.tint
            && rotation == other.rotation && surfaceId == other.surfaceId && sameMaterial(material, other.material);
    }
    bool          known() const { return (color >> 24) != 0; }
    bool          floor() const { return known() && flags == 0; }
};
struct MapTile {
    std::array<MapCell, 256> cells{};
    std::uint64_t            lastTouched{};
    BiomeTile               biomes;
};
struct TileRecord {
    TileKey key;
    MapTile tile;
};
struct MapBounds {
    double minX{}, minZ{}, maxX{}, maxZ{};
};
// A small height-only halo; material snapshots are never copied just to shade
// neighbouring terrain. Four blocks separate hill shape from single-block steps.
struct TerrainHeight {
    int height{};
    std::uint8_t depth{};
    bool floor{};
    TerrainHeight() = default;
    TerrainHeight(MapCell const& cell) : height(cell.height), depth(cell.depth), floor(cell.floor()) {}
    bool land() const { return floor && !depth; }
};
class TerrainNeighborhood {
public:
    static constexpr int radius = 4, side = 16 + 2 * radius;
    static constexpr std::array<std::array<int, 2>, 4> directions{{{0, -1}, {-1, 0}, {0, 1}, {1, 0}}};
    explicit TerrainNeighborhood(MapTile const& tile) {
        for (int z = 0; z < 16; ++z) for (int x = 0; x < 16; ++x) at(x, z) = tile.cells[z * 16 + x];
    }
    void edge(int direction, MapTile const& tile) {
        for (int d = 1; d <= radius; ++d) for (int p = 0; p < 16; ++p) {
            if (direction == 0) at(p, -d) = tile.cells[(16 - d) * 16 + p];
            if (direction == 1) at(-d, p) = tile.cells[p * 16 + 16 - d];
            if (direction == 2) at(p, 15 + d) = tile.cells[(d - 1) * 16 + p];
            if (direction == 3) at(15 + d, p) = tile.cells[p * 16 + d - 1];
        }
    }
    TerrainHeight const& at(int x, int z) const { return mHeights[(z + radius) * side + x + radius]; }
private:
    TerrainHeight& at(int x, int z) { return mHeights[(z + radius) * side + x + radius]; }
    std::array<TerrainHeight, side * side> mHeights{};
};
inline void retainPendingMaterial(MapCell& cell, MapCell const& previous) {
    // A budget-limited/async image lookup must not flash an unchanged block back
    // to a flat colour. A different state, height or liquid never inherits it.
    if (!cell.material && cell.floor() && !cell.depth && previous.material
        && cell.surfaceId && cell.surfaceId == previous.surfaceId && cell.height == previous.height) {
        cell.material = previous.material;
        cell.rotation = previous.rotation;
        cell.color = tintTopTexture(cell.material->average(), cell.tint);
    }
}
struct MapLighting {
    bool enabled{};
    int skyDarken{};
    bool operator==(MapLighting const&) const = default;
};
inline std::array<float, 3> lightFactors(MapCell const& cell, MapLighting lighting) {
    if (!lighting.enabled || !cell.floor() || cell.skyLight > 15 || cell.blockLight > 15) return {1, 1, 1};
    int sky = std::max(0, int(cell.skyLight) - std::clamp(lighting.skyDarken, 0, 15));
    int light = std::max(sky, int(cell.blockLight));
    // Retain readable terrain in darkness. Block-lit areas keep their warm colour;
    // unlit night/cave terrain gets a subtle cool tint.
    // Lift shadows and midtones for map readability; the unadjusted world-light
    // curve compounded with relief made night terrain nearly indistinguishable.
    float brightness = 0.32f + 0.68f * std::sqrt(float(light) / (60.0f - 3.0f * light));
    float cool = (1.0f - float(light) / 15.0f) * (1.0f - float(cell.blockLight) / 15.0f);
    return {brightness * (1.0f - 0.16f * cool), brightness * (1.0f - 0.08f * cool), brightness};
}
inline std::uint32_t applyLight(std::uint32_t color, std::array<float, 3> const& factors) {
    auto channel = [&](int shift) { return static_cast<unsigned>(((color >> shift) & 255u) * factors[shift / 8] + 0.5f); };
    return rgba(channel(0), channel(8), channel(16));
}
inline std::uint32_t litColor(std::uint32_t color, MapCell const& cell, MapLighting lighting) {
    if (!cell.known()) return color;
    return applyLight(color, lightFactors(cell, lighting));
}
inline constexpr double minMapBlocksPerPixel = 0.125;
struct MapView {
    double centerX{}, centerZ{};
    double blocksPerPixel{2.0};
    int    width{128}, height{128};

    std::array<double, 2> worldAt(double pixelX, double pixelY) const {
        return {centerX + (pixelX - width * 0.5) * blocksPerPixel, centerZ + (pixelY - height * 0.5) * blocksPerPixel};
    }
    void zoomAt(double factor, double pixelX, double pixelY) {
        auto before     = worldAt(pixelX, pixelY);
        blocksPerPixel  = std::clamp(blocksPerPixel * factor, minMapBlocksPerPixel, 128.0);
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
inline float terrainShade(MapCell const& cell, TerrainHeight north, TerrainHeight west,
                          TerrainHeight south = {}, TerrainHeight east = {},
                          TerrainHeight northFar = {}, TerrainHeight westFar = {},
                          TerrainHeight southFar = {}, TerrainHeight eastFar = {}) {
    if (cell.flags) return 1.0f;
    float shade;
    if (cell.depth > 0) {
        // Continuous depth shading makes shores and deep channels distinguishable.
        shade = 1.08f - 0.50f * cell.depth / (cell.depth + 10.0f);
    } else {
        auto gradient = [&](TerrainHeight before, TerrainHeight after, float distance, float fallback) {
            int count = int(before.land()) + int(after.land());
            if (!count) return fallback;
            float difference = (before.land() ? float(cell.height - before.height) : 0.0f)
                + (after.land() ? float(after.height - cell.height) : 0.0f);
            return difference / (count * distance);
        };
        auto slope = [&](TerrainHeight before, TerrainHeight after, TerrainHeight farBefore, TerrainHeight farAfter) {
            float near = gradient(before, after, 1, 0);
            // Fall back on each side independently. A flat far sample on the
            // other side must not erase the gradient of a nearby cliff.
            float beforeSlope = farBefore.land() ? (cell.height - farBefore.height) / 4.0f
                : before.land() ? float(cell.height - before.height) : 0.0f;
            float afterSlope = farAfter.land() ? (farAfter.height - cell.height) / 4.0f
                : after.land() ? float(after.height - cell.height) : 0.0f;
            int count = int(before.land() || farBefore.land()) + int(after.land() || farAfter.land());
            float broad = count ? (beforeSlope + afterSlope) / count : near;
            float difference = 0.3f * near + 0.7f * broad;
            return std::copysign(std::log2(1.0f + std::abs(difference)), difference);
        };
        // Central slopes illuminate both sides of a hill. The wider sample makes
        // terraces read as one hillside instead of disconnected one-block bands.
        // Symmetric peaks have zero central slope. Local convexity separates
        // summits/cliff lips from hollows/cliff feet even across the light axis.
        // Require opposite samples so fog borders cannot invent a ridge.
        auto curvature = [&](TerrainHeight before, TerrainHeight after, float distance, float fallback) {
            if (!before.land() || !after.land()) return fallback;
            return (2.0f * cell.height - before.height - after.height) / (2.0f * distance);
        };
        auto shape = [&](TerrainHeight before, TerrainHeight after, TerrainHeight farBefore, TerrainHeight farAfter) {
            float near = curvature(before, after, 1, 0);
            float broad = curvature(farBefore, farAfter, 4, near);
            return 0.45f * near + 0.55f * broad;
        };
        float position = 0.5f * (shape(north, south, northFar, southFar)
            + shape(west, east, westFar, eastFar));
        float prominence = 0.12f * position / (1.0f + std::abs(position));
        float occlusion = 0;
        for (auto neighbor : {north, west, south, east})
            if (neighbor.land()) occlusion += std::clamp((neighbor.height - cell.height - 1.0f) / 12.0f, 0.0f, 1.0f);
        float altitude = std::clamp(1.0f + (cell.height - 64) * 0.0005f, 0.95f, 1.06f);
        float directional = 0.18f * slope(north, south, northFar, southFar)
            + 0.13f * slope(west, east, westFar, eastFar);
        // Leave headroom for shape contrast before the final material-safe clamp:
        // a sun-facing cliff's top and bottom must not both clip to white.
        float relief = 0.20f * directional / std::sqrt(0.04f + directional * directional)
            + prominence - 0.025f * occlusion;
        shade = std::clamp(1.0f + relief, 0.72f, 1.22f) * altitude;
    }
    return shade;
}
inline std::uint32_t shadePixel(std::uint32_t color, float shade) {
    auto channel = [&](int shift) {
        return static_cast<unsigned>(std::clamp(((color >> shift) & 255u) * shade, 0.0f, 255.0f));
    };
    return rgba(channel(0), channel(8), channel(16));
}
inline std::uint32_t shadedColor(MapCell const& cell, MapCell const& north, MapCell const& west) {
    return cell.known() ? shadePixel(cell.color, terrainShade(cell, north, west)) : 0;
}
inline float terrainShadeAt(MapCell const& cell, TerrainNeighborhood const& heights, int x, int z) {
    auto continuousFar = [&](int dx, int dz) {
        int previous = cell.height;
        TerrainHeight sample;
        for (int distance = 1; distance <= TerrainNeighborhood::radius; ++distance) {
            sample = heights.at(x + dx * distance, z + dz * distance);
            // Do not smear a roof/cliff onto flat ground or bridge water/fog.
            // Broad relief follows connected gentle steps, not height alone.
            if (!sample.land() || std::abs(sample.height - previous) > 3) return TerrainHeight{};
            previous = sample.height;
        }
        return sample;
    };
    return terrainShade(cell, heights.at(x, z - 1), heights.at(x - 1, z), heights.at(x, z + 1), heights.at(x + 1, z),
        continuousFar(0, -1), continuousFar(-1, 0), continuousFar(0, 1), continuousFar(1, 0));
}
inline ColorAggregate tileOverview(MapTile const& tile, MapLighting lighting = {}) {
    ColorAggregate result;
    TerrainNeighborhood heights(tile);
    for (int i = 0; i < 256; ++i)
        if (tile.cells[i].known())
            result.add(litColor(shadePixel(tile.cells[i].color, terrainShadeAt(tile.cells[i], heights, i % 16, i / 16)),
                tile.cells[i], lighting));
    return result;
}
// Area filtering for overview; block-aligned samples and narrow relief edges for close-ups.
// Unknown cells have zero weight, so small explored islands remain visible in overview.
class MapRaster {
public:
    explicit MapRaster(MapView view, MapLighting lighting = {})
    : mView(view),
      mOrigin(view.worldAt(0, 0)),
      mColors(static_cast<std::size_t>(view.width) * view.height), mLighting(lighting) {}
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
    void tile(TileKey key, MapTile const& tile, TerrainNeighborhood const& heights) {
        if (!intersects(key)) return;
        int step = 1;
        while (step < 16 && step * 2 <= mView.blocksPerPixel / 2) step *= 2;
        for (int z = 0; z < 16; z += step)
            for (int x = 0; x < 16; x += step) {
                ColorAggregate color;
                for (int dz = 0; dz < step; ++dz)
                    for (int dx = 0; dx < step; ++dx) {
                        int  index = (z + dz) * 16 + x + dx;
                        auto const& cell  = tile.cells[index];
                        if (!cell.known()) continue;
                        float shade = terrainShadeAt(cell, heights, x + dx, z + dz);
                        if (mView.blocksPerPixel <= 1.0) {
                            detailCell(key.x * 16.0 + x + dx, key.z * 16.0 + z + dz, cell, shade,
                                heights.at(x + dx, z + dz - 1), heights.at(x + dx - 1, z + dz),
                                heights.at(x + dx, z + dz + 1), heights.at(x + dx + 1, z + dz));
                            continue;
                        }
                        color.add(litColor(shadePixel(cell.color, shade), cell, mLighting));
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
                if (c.count > 0) {
                    // Tile iteration order can put an exact half-channel just
                    // below the tie. Stabilise quantisation for live/history maps.
                    auto channel = [&](double sum) { return static_cast<unsigned>(sum / c.count + 0.5 + 1e-9); };
                    pixels[index] = rgba(channel(c.r), channel(c.g), channel(c.b));
                } else {
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
    void detailCell(double x, double z, MapCell const& cell, float relief,
                    TerrainHeight north, TerrainHeight west, TerrainHeight south, TerrainHeight east) {
        double scale = mView.blocksPerPixel;
        double left = (x - mOrigin[0]) / scale, top = (z - mOrigin[1]) / scale;
        // Half-open block bounds assign each pixel centre exactly once, even at
        // negative coordinates and chunk seams. No second area blur at close zoom.
        int x0 = std::max(0, static_cast<int>(std::ceil(left - 0.5)));
        int z0 = std::max(0, static_cast<int>(std::ceil(top - 0.5)));
        int x1 = std::min(mView.width, static_cast<int>(std::ceil((x + 1.0 - mOrigin[0]) / scale - 0.5)));
        int z1 = std::min(mView.height, static_cast<int>(std::ceil((z + 1.0 - mOrigin[1]) / scale - 0.5)));
        auto color = litColor(shadePixel(cell.color, relief), cell, mLighting);
        bool textured = cell.material && cell.floor() && !cell.depth && scale < 1.0;
        auto light = lightFactors(cell, mLighting);
        // Blend adjacent mip levels so wheel zoom does not abruptly swap patterns.
        double lod = std::clamp(std::log2(std::max(scale, 1.0 / 16) * 16), 0.0, 4.0);
        int fine = int(lod), coarse = std::min(fine + 1, 4);
        double blend = lod - fine;
        auto edge = [&](TerrainHeight neighbor) {
            if (!cell.floor() || !neighbor.floor) return 0.0;
            // Only the water side outlines a shoreline. Deep water has no grid.
            if (cell.depth || neighbor.depth) return cell.depth && !neighbor.depth ? 0.18 : 0.0;
            int height = std::abs(int(cell.height) - int(neighbor.height));
            return height ? std::min(0.26, 0.12 + 0.04 * std::log2(1.0 + height)) : 0.0;
        };
        double northEdge = edge(north), westEdge = edge(west);
        double southEdge = edge(south), eastEdge = edge(east);
        // Fade in only when there is enough resolution for a 1/8-block edge.
        // These edges come from sampled heights/water, never generated texture noise.
        double strength = std::clamp((0.5 - scale) * 4.0, 0.0, 1.0);
        for (int row = z0; row < z1; ++row) {
            double northCoverage = std::clamp(0.125 / scale - (row - top), 0.0, 1.0);
            double southCoverage = std::clamp(0.125 / scale - (top + 1.0 / scale - row - 1), 0.0, 1.0);
            for (int col = x0; col < x1; ++col) {
                double westCoverage = std::clamp(0.125 / scale - (col - left), 0.0, 1.0);
                double eastCoverage = std::clamp(0.125 / scale - (left + 1.0 / scale - col - 1), 0.0, 1.0);
                double shade = 1.0 - strength * std::max({northEdge * northCoverage, westEdge * westCoverage,
                    southEdge * southCoverage, eastEdge * eastCoverage});
                auto pixel = color;
                if (textured) {
                    double u = (col + 0.5 - left) * scale, v = (row + 0.5 - top) * scale;
                    auto a = materialPixel(cell.material->texel(fine, u, v, cell.rotation), cell.tint, cell.color);
                    auto b = materialPixel(cell.material->texel(coarse, u, v, cell.rotation), cell.tint, cell.color);
                    auto channel = [&](int shift) {
                        return unsigned(((a >> shift) & 255u) * (1.0 - blend) + ((b >> shift) & 255u) * blend + 0.5);
                    };
                    pixel = applyLight(shadePixel(rgba(channel(0), channel(8), channel(16)), relief), light);
                }
                auto& dest = mColors[static_cast<std::size_t>(row) * mView.width + col];
                dest = {double(pixel & 255u) * shade, double((pixel >> 8) & 255u) * shade,
                        double((pixel >> 16) & 255u) * shade, 1.0};
            }
        }
    }

    MapView                     mView;
    std::array<double, 2>       mOrigin;
    std::vector<ColorAggregate> mColors;
    MapLighting                 mLighting;
};

class MapCache {
public:
    explicit MapCache(std::size_t capacity = 8192) : mCapacity(std::max<std::size_t>(1, capacity)) {}
    MapCell get(MapLayer layer, int x, int z) const {
        auto it = mTiles.find({layer.dimension, floorDiv(x, 16), floorDiv(z, 16), layer.slice});
        if (it == mTiles.end()) return {};
        return it->second.cells[localBlock(z) * 16 + localBlock(x)];
    }
    std::string_view biome(MapLayer layer, int x, int z) const {
        auto it = mTiles.find({layer.dimension, floorDiv(x, 16), floorDiv(z, 16), layer.slice});
        return it == mTiles.end() ? std::string_view{} : it->second.biomes.get(localBlock(z) * 16 + localBlock(x));
    }
    template <class Visitor> void visitTiles(MapLayer layer, Visitor&& visit) const {
        for (auto const& [key, tile] : mTiles) if (key.layer() == layer) visit(key, tile);
    }
    bool put(MapLayer layer, int x, int z, MapCell cell, std::string_view biome = {}) {
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
        auto& previous = it->second.cells[localBlock(z) * 16 + localBlock(x)];
        bool biomeChanged = it->second.biomes.set(localBlock(z) * 16 + localBlock(x), biome);
        if (previous == cell && !biomeChanged) return false;
        previous = cell;
        mDirty.insert(key);
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
                for (std::size_t i = 0; i < record.tile.cells.size(); ++i) {
                    if (!it->second.cells[i].known()) it->second.cells[i] = record.tile.cells[i];
                    else retainPendingMaterial(it->second.cells[i], record.tile.cells[i]);
                    if (it->second.biomes.get(i).empty()) it->second.biomes.set(i, record.tile.biomes.get(i));
                }
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

    std::vector<std::uint32_t> rasterize(MapLayer layer, MapView const& view, MapLighting lighting = {}) const {
        MapRaster raster(view, lighting);
        for (auto const& [key, tile] : mTiles)
            if (key.layer() == layer && raster.intersects(key)) {
                if (view.blocksPerPixel >= 32) {
                    raster.add(key.x * 16.0, key.z * 16.0, 16, tileOverview(tile, lighting));
                    continue;
                }
                TerrainNeighborhood heights(tile);
                for (int direction = 0; direction < 4; ++direction) {
                    auto offset = TerrainNeighborhood::directions[direction];
                    auto neighbor = mTiles.find({key.dimension, key.x + offset[0], key.z + offset[1], key.slice});
                    if (neighbor != mTiles.end()) heights.edge(direction, neighbor->second);
                }
                raster.tile(key, tile, heights);
            }
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
