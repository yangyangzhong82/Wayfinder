#pragma once

#include "wayfinder/Navigation.h"

namespace wayfinder {
// Each cell includes its backdrop and optional target outline. Baking the palette
// lets every marker share one texture/material and one flush, regardless of color.
struct MarkerAtlas {
    static constexpr int tileSize       = 16;
    static constexpr int iconCount      = static_cast<int>(markerIcons.size());
    static constexpr int directionCount = 16;
    static constexpr int columns        = iconCount + directionCount;
    static constexpr int rows           = static_cast<int>(markerColors.size()) * 2;
    static constexpr int width          = columns * tileSize;
    static constexpr int height         = rows * tileSize;

    static int                        arrowIcon(double dx, double dy);
    static std::array<float, 2>       uv(int icon, int color, bool target);
    static std::vector<std::uint32_t> pixels();
};
} // namespace wayfinder
