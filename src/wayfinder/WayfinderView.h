#pragma once
#include "wayfinder/MapUi.h"
#include "wayfinder/Settings.h"

namespace wayfinder {
// Raster data stays on a stable texel grid; the visible minimap moves inside it.
MapView minimapTextureView(MapView visible);
bool textureCoversView(MapView const& texture, MapView const& visible, double marginPixels = 0);
MapRect textureUvRect(MapView const& texture, MapView const& visible);

// GUI/world transforms only. No engine objects, input ownership, or disk jobs.
struct WayfinderView {
    bool fullscreen{}, following{true};
    float guiWidth{}, guiHeight{};
    MapRect area;
    MapView fullView;

    void follow(double playerX, double playerZ);
    void fit(std::optional<MapBounds> live, std::optional<MapBounds> history);
    void zoom(double factor, bool hasPointer, float pointerX, float pointerY);
    void drag(float dx, float dy);
    MapView layout(float width, float height, Settings const& settings, double playerX, double playerZ);
};
} // namespace wayfinder
