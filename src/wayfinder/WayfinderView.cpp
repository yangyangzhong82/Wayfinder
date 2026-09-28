#include "wayfinder/WayfinderView.h"

namespace wayfinder {
MapView minimapTextureView(MapView visible) {
    // Leave room for motion between async updates, plus a texel for filtering.
    constexpr int border = 8;
    visible.centerX = std::floor(visible.centerX / visible.blocksPerPixel) * visible.blocksPerPixel;
    visible.centerZ = std::floor(visible.centerZ / visible.blocksPerPixel) * visible.blocksPerPixel;
    visible.width += border * 2;
    visible.height += border * 2;
    return visible;
}

bool textureCoversView(MapView const& texture, MapView const& visible, double marginPixels) {
    if (texture.blocksPerPixel != visible.blocksPerPixel || texture.blocksPerPixel <= 0) return false;
    double roomX = (texture.width - visible.width) * 0.5 - marginPixels;
    double roomZ = (texture.height - visible.height) * 0.5 - marginPixels;
    return std::abs(visible.centerX - texture.centerX) / texture.blocksPerPixel <= roomX
        && std::abs(visible.centerZ - texture.centerZ) / texture.blocksPerPixel <= roomZ;
}

MapRect textureUvRect(MapView const& texture, MapView const& visible) {
    // Difference first preserves sub-block precision near the world boundary.
    double x = (visible.centerX - texture.centerX) / texture.blocksPerPixel + (texture.width - visible.width) * 0.5;
    double z = (visible.centerZ - texture.centerZ) / texture.blocksPerPixel + (texture.height - visible.height) * 0.5;
    return {static_cast<float>(x / texture.width), static_cast<float>(z / texture.height),
            static_cast<float>(visible.width) / texture.width, static_cast<float>(visible.height) / texture.height};
}

void WayfinderView::follow(double playerX, double playerZ) {
    following = true;
    fullView.centerX = playerX;
    fullView.centerZ = playerZ;
}

void WayfinderView::fit(std::optional<MapBounds> bounds, std::optional<MapBounds> history) {
    if (history) {
        if (!bounds) bounds = history;
        else {
            bounds->minX = std::min(bounds->minX, history->minX);
            bounds->minZ = std::min(bounds->minZ, history->minZ);
            bounds->maxX = std::max(bounds->maxX, history->maxX);
            bounds->maxZ = std::max(bounds->maxZ, history->maxZ);
        }
    }
    if (!bounds) return;
    fullView.centerX = (bounds->minX + bounds->maxX) * 0.5;
    fullView.centerZ = (bounds->minZ + bounds->maxZ) * 0.5;
    fullView.blocksPerPixel = std::clamp(
        1.1 * std::max((bounds->maxX - bounds->minX) / fullView.width, (bounds->maxZ - bounds->minZ) / fullView.height),
        0.5, 128.0);
    following = false;
}

void WayfinderView::zoom(double factor, bool hasPointer, float pointerX, float pointerY) {
    double x = fullView.width * 0.5, y = fullView.height * 0.5;
    if (hasPointer && area.contains(pointerX, pointerY)) {
        x = (pointerX - area.x) / area.width * fullView.width;
        y = (pointerY - area.y) / area.height * fullView.height;
        following = false;
    }
    fullView.zoomAt(factor, x, y);
}

void WayfinderView::drag(float dx, float dy) {
    if (area.width <= 0 || area.height <= 0) return;
    fullView.centerX -= dx / area.width * fullView.width * fullView.blocksPerPixel;
    fullView.centerZ -= dy / area.height * fullView.height * fullView.blocksPerPixel;
    following = false;
}

MapView WayfinderView::layout(float width, float height, Settings const& settings, double playerX, double playerZ) {
    guiWidth = width;
    guiHeight = height;
    MapView view;
    if (fullscreen) {
        area = fullMapArea(width, height);
        fullView.width = settings.fullscreenPixels;
        fullView.height = std::clamp(static_cast<int>(fullView.width * area.height / area.width), 64, 768);
        if (following) follow(playerX, playerZ);
        fullView.centerX = std::clamp(fullView.centerX, -29999984.0, 29999984.5);
        fullView.centerZ = std::clamp(fullView.centerZ, -29999984.0, 29999984.5);
        view = fullView;
    } else {
        float side = std::min({settings.minimapSize, width * 0.40f, height - 74.0f});
        area = {9 + std::max(0.0f, width - side - 18) * settings.minimapPositionX,
                20 + std::max(0.0f, height - side - 74) * settings.minimapPositionY, side, side};
        view = {playerX, playerZ, settings.minimapBlocksPerPixel, settings.minimapPixels, settings.minimapPixels};
    }
    // Fullscreen keeps its existing sampling behavior. The minimap's visible
    // center remains exact; only its separate padded texture is texel-aligned.
    if (fullscreen && following) {
        view.centerX = std::floor(view.centerX / view.blocksPerPixel) * view.blocksPerPixel;
        view.centerZ = std::floor(view.centerZ / view.blocksPerPixel) * view.blocksPerPixel;
    }
    return view;
}
} // namespace wayfinder
