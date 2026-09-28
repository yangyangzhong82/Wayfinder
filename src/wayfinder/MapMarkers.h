#pragma once
#include "wayfinder/MapUi.h"
#include "wayfinder/Navigation.h"

namespace wayfinder {
struct PlacedMarker {
    std::uint64_t id{};
    float x{}, y{};
    MarkerProjection projection;
    bool target{};
    std::optional<MapRect> label;
};
// Shared draw/hit geometry. Labels may move or hide; geographic icon positions never move.
std::vector<PlacedMarker> layoutMarkers(Navigation const& nav, MapView const& view, MapRect const& area,
                                       int dimension, bool showWaypoints, bool showNavigation, bool labels);
std::uint64_t hitMarker(std::vector<PlacedMarker> const& markers, float x, float y);
} // namespace wayfinder
