#pragma once
#include "wayfinder/MapCore.h"
#include <string_view>

namespace wayfinder {
inline MapLayer terrainLayer(int dimension, int playerY, int caveSwitchY, std::string_view mode, MapLayer previous) {
    bool cave = mode == "cave" || (mode != "surface" && dimension == 1);
    if (mode == "auto" && dimension == 0) {
        bool wasCave = previous.dimension == dimension && previous.underground();
        // The setting is an absolute world height. A four-block return margin
        // prevents walking/jumping near the threshold from toggling every tick.
        cave = wasCave ? static_cast<std::int64_t>(playerY) < static_cast<std::int64_t>(caveSwitchY) + 4
                       : playerY <= caveSwitchY;
    }
    return {dimension, cave ? floorDiv(std::clamp(playerY, -32768, 32767), caveSliceHeight) : surfaceSlice};
}

enum class ColumnKind { Air, Solid, Water, Lava };
struct ColumnBlock {
    ColumnKind    kind{ColumnKind::Air};
    std::uint32_t color{};
    bool          waterlogged{};
    TerrainMaterialPtr material{};
    std::uint32_t tint{0xffffffffu};
    std::uint8_t rotation{};
    std::uint64_t surfaceId{};
};
inline MapCell emptyTerrain(int y, bool wall) {
    return {
        wall ? rgba(38, 40, 46) : rgba(13, 12, 23),
        static_cast<std::int16_t>(std::clamp(y, -32768, 32767)),
        0,
        wall ? MapCell::wall : MapCell::voidSpace
    };
}
// The reader returns nullopt for unavailable client data, never air. Shared by
// surface/cave sampling and engine-independent tests. All scans are bounded.
template <class Read>
std::optional<MapCell> columnFloor(int top, int minY, Read&& read) {
    std::optional<MapCell> water;
    int                    y = top;
    for (; y >= minY && top - y < 64; --y) {
        auto block = read(y);
        if (!block) return water;
        if (block->kind == ColumnKind::Water) {
            if (!water) water = MapCell{block->color, static_cast<std::int16_t>(y)};
            water->depth = static_cast<std::uint8_t>(std::min(water->height - y + 1, 255));
            if (block->waterlogged) return water;
            continue;
        }
        if (water) return water;
        if (block->kind == ColumnKind::Air) continue;
        return MapCell{block->color, static_cast<std::int16_t>(y), 0, 0, 255, 255,
            block->material, block->tint, block->rotation, block->surfaceId};
    }
    if (water) return water;
    return y < minY ? std::optional<MapCell>(emptyTerrain(minY, false)) : std::nullopt;
}
// The engine's solid-height hint can be the bottom of a lava pool. Resolve any
// liquid directly above it before scanning down, without looking through roofs
// or treating an unavailable liquid surface as the solid floor underneath.
template <class Read>
std::optional<MapCell> surfaceColumn(int solidTop, int minY, int maxY, Read&& read) {
    if (minY >= maxY || solidTop < minY - 1 || solidTop >= maxY) return {};
    int top = solidTop;
    while (top + 1 < maxY) {
        auto above = read(top + 1);
        if (!above) return {};
        if (above->kind != ColumnKind::Water && above->kind != ColumnKind::Lava) break;
        if (top - solidTop >= 64) return {};
        ++top;
        // A waterlogged solid is itself a visible surface, not a passage upward.
        if (above->waterlogged) break;
    }
    return columnFloor(top, minY, read);
}
template <class Read>
std::optional<MapCell> caveColumn(int referenceY, int minY, int maxY, Read&& read) {
    if (minY >= maxY) return {};
    int anchor = std::clamp(referenceY, minY, maxY - 1);
    // Find the closest open space within this slice. Solid columns remain walls
    // instead of revealing an unrelated cavern far above/below the player.
    for (int distance = 0; distance <= caveSliceHeight / 2; ++distance) {
        for (int direction : {-1, 1}) {
            if (distance == 0 && direction == 1) continue;
            int y = anchor + distance * direction;
            if (y < minY || y >= maxY) continue;
            auto block = read(y);
            if (!block) return {};
            if (block->kind == ColumnKind::Solid) continue;
            if (block->kind == ColumnKind::Water || block->kind == ColumnKind::Lava) {
                // A player may be below a lake's surface. Resolve the actual
                // liquid top instead of treating the slice centre as its height.
                int bottom = y;
                while (y + 1 < maxY) {
                    auto above = read(y + 1);
                    if (!above) return {};
                    if (above->kind != block->kind) break;
                    if (y - bottom >= 63) return {};
                    ++y;
                }
            }
            return columnFloor(y, minY, read);
        }
    }
    return emptyTerrain(anchor, true);
}
} // namespace wayfinder
