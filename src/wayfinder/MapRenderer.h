#pragma once

#include "wayfinder/MapCore.h"
#include "wayfinder/EntityRadar.h"
#include "wayfinder/MapUi.h"
#include "wayfinder/Navigation.h"
#include "wayfinder/Settings.h"
#include <memory>
#include <string>

class MinecraftUIRenderContext;
namespace wayfinder {
class MapRenderer {
public:
    MapRenderer();
    ~MapRenderer();
    void render(
        MinecraftUIRenderContext&         context,
        MapRect const&                    area,
        MapView const&                    view,
        MapView const&                    textureView,
        std::vector<std::uint32_t> const& pixels,
        std::uint64_t                     imageRevision,
        double                            playerX,
        double                            playerZ,
        float                             yaw,
        std::string const&                caption,
        bool                              fullscreen,
        Settings const&                   settings,
        Locale const&                     locale,
        bool                              following,
        std::string const&                biome,
        std::string const&                layerLabel
    );
    void renderMarkers(
        MinecraftUIRenderContext& context,
        MapRect const&            area,
        MapView const&            view,
        Navigation const&         navigation,
        int                       dimension,
        double                    playerX,
        double                    playerZ,
        bool                      fullscreen,
        Settings const&           settings,
        Locale const&             locale
    );
    void renderUi(MinecraftUIRenderContext& context, UiFrame const& frame, float mouseX, float mouseY);
    void renderEntities(MinecraftUIRenderContext& context, MapRect const& area, MapView const& view,
                        std::span<EntityMarker const> entities, MapLayer layer, double playerX, double playerY,
                        double playerZ, Settings const& settings, Locale const& locale, bool fullscreen);
    // Called at a UI callback, or while the mod is being disabled on the client thread.
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
} // namespace wayfinder
