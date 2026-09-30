#pragma once
#include "wayfinder/MapUi.h"
#include <string>

namespace wayfinder {
struct MapInspectionPoint {
    MapLayer layer;
    int x{}, z{};
    bool operator==(MapInspectionPoint const&) const = default;
};
inline std::optional<MapInspectionPoint> mapInspectionPoint(
    MapView const& view, MapRect const& area, MapLayer layer, float mouseX, float mouseY
) {
    if (!area.contains(mouseX, mouseY)) return {};
    auto world = view.worldAt((mouseX - area.x) / area.width * view.width,
                              (mouseY - area.y) / area.height * view.height);
    if (!std::isfinite(world[0]) || !std::isfinite(world[1])
        || std::abs(world[0]) > 29999984 || std::abs(world[1]) > 29999984) return {};
    return MapInspectionPoint{layer, blockCoordinate(world[0]), blockCoordinate(world[1])};
}
struct CursorBiome {
    std::optional<MapInspectionPoint> point;
    std::string identifier;
    bool historyRead{};
    void select(std::optional<MapInspectionPoint> next) {
        if (point != next) { identifier.clear(); historyRead = false; } // Never display the previous block/layer's biome.
        point = next;
    }
};
} // namespace wayfinder
