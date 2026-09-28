#pragma once
#include "wayfinder/MapCore.h"

namespace wayfinder {
struct ScaleBar {
    double blocks{}, pixels{};
};
inline ScaleBar mapScale(MapView const& view, double guiWidth) {
    double blocksPerGui = view.width * view.blocksPerPixel / guiWidth;
    double maximum      = std::min(80.0, guiWidth * 0.3) * blocksPerGui;
    double magnitude    = std::pow(10.0, std::floor(std::log10(maximum)));
    double nice         = maximum / magnitude >= 5 ? 5 : maximum / magnitude >= 2 ? 2 : 1;
    double blocks       = nice * magnitude;
    return {blocks, blocks / blocksPerGui};
}
// Do not turn distant maps into a solid grid. At close scales these are true
// 16-block chunk edges (including negative coordinates), never coarser fake edges.
inline std::vector<double> chunkLines(double origin, double blocksPerGui, double extent) {
    std::vector<double> lines;
    if (blocksPerGui <= 0 || 16 / blocksPerGui < 6) return lines;
    double first = std::ceil(origin / 16.0) * 16.0;
    for (double world = first; world < origin + extent * blocksPerGui; world += 16)
        lines.push_back((world - origin) / blocksPerGui);
    return lines;
}
inline void locateView(MapView& view, int x, int z) {
    view.centerX = x + 0.5;
    view.centerZ = z + 0.5;
}
} // namespace wayfinder
