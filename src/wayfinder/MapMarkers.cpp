#include "wayfinder/MapMarkers.h"

namespace wayfinder {
std::vector<PlacedMarker> layoutMarkers(Navigation const& nav, MapView const& view, MapRect const& area,
                                       int dimension, bool showWaypoints, bool showNavigation, bool labels) {
    std::vector<PlacedMarker> result;
    if (area.width <= 0 || area.height <= 0) return result;
    auto add = [&](Waypoint const& point, bool target) {
        if (point.dimension != dimension) return;
        auto p = projectMarker(view, point.x + 0.5, point.z + 0.5, area.width, area.height);
        if (p.outside && !target) return;
        result.push_back({point.id, area.x + float(p.x), area.y + float(p.y), p, target, {}});
    };
    if (showWaypoints)
        for (auto const& p : nav.points) if (p.id != nav.target || !showNavigation) add(p, false);
    if (showNavigation) if (auto target = nav.find(nav.target)) add(*target, true);
    if (!labels) return result;
    std::vector<MapRect> occupied;
    for (auto const& p : result) occupied.push_back({p.x - 9, p.y - 9, 18, 18});
    // Target and topmost icons get first choice of label positions.
    for (auto it = result.rbegin(); it != result.rend(); ++it) {
        if (it->projection.outside) continue;
        auto point = nav.find(it->id);
        float textWidth = 8;
        for (unsigned char ch : point->name)
            if ((ch & 0xc0) != 0x80) textWidth += ch < 0x80 ? 4.5f : 7.0f;
        textWidth = std::clamp(textWidth, 36.0f, 140.0f);
        std::array<MapRect, 4> candidates{{
            {it->x + 11, it->y - 5, textWidth, 14},
            {it->x - 11 - textWidth, it->y - 5, textWidth, 14},
            {it->x - textWidth / 2, it->y - 25, textWidth, 14},
            {it->x - textWidth / 2, it->y + 11, textWidth, 14}}};
        for (auto rect : candidates) {
            if (rect.x < area.x || rect.y < area.y || rect.x + rect.width > area.x + area.width
                || rect.y + rect.height > area.y + area.height) continue;
            if (std::any_of(occupied.begin(), occupied.end(), [&](auto const& other) { return rect.intersects(other); })) continue;
            it->label = rect;
            occupied.push_back(rect);
            break;
        }
    }
    return result;
}
std::uint64_t hitMarker(std::vector<PlacedMarker> const& markers, float x, float y) {
    for (auto it = markers.rbegin(); it != markers.rend(); ++it)
        if (MapRect{it->x - 8, it->y - 8, 16, 16}.contains(x, y)) return it->id;
    return 0;
}
} // namespace wayfinder
