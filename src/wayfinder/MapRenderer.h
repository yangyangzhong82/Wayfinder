#pragma once

#include "wayfinder/MapCore.h"
#include <memory>
#include <string>

class MinecraftUIRenderContext;
namespace wayfinder {
struct MapRect {
    float x{}, y{}, width{}, height{};
    bool  contains(float px, float py) const { return px >= x && py >= y && px < x + width && py < y + height; }
};
class MapRenderer {
public:
    MapRenderer();
    ~MapRenderer();
    void render(
        MinecraftUIRenderContext&         context,
        MapRect const&                    area,
        MapView const&                    view,
        std::vector<std::uint32_t> const& pixels,
        std::uint64_t                     imageRevision,
        double                            playerX,
        double                            playerZ,
        float                             yaw,
        std::string const&                caption,
        bool                              fullscreen
    );
    // HUD overlays do not automatically acquire the native screen cursor.
    void renderCursor(MinecraftUIRenderContext& context, float x, float y, bool dragging);
    // Called at a UI callback, or while the mod is being disabled on the client thread.
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
} // namespace wayfinder
