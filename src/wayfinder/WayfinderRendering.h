#pragma once
#include "wayfinder/MapRenderer.h"
#include "wayfinder/WayfinderView.h"
#include <chrono>
#include <future>

namespace wayfinder {
// Borrowed read-only data, valid for one synchronous UI callback. Never queued
// to a worker or retained by the renderer.
struct MapFrame {
    Settings const& settings;
    Locale const& locale;
    Navigation const& navigation;
    ExplorationTrail const& trail;
    std::span<EntityMarker const> entities;
    UiFrame const* ui{};
    MapRect area;
    MapLayer layer, liveLayer;
    double playerX{}, playerY{}, playerZ{};
    float yaw{}, guiWidth{}, guiHeight{}, mouseX{}, mouseY{};
    int playerDimension{};
    bool fullscreen{}, following{};
    std::string caption, biome, cursorInfo, layerLabel;
};

class WayfinderRendering {
public:
    using Clock = std::chrono::steady_clock;
    // Returns a desired history view; the coordinator decides when to submit it.
    std::optional<MapView> refreshImage(MapView const& view, MapLayer layer, Settings const& settings,
        MapCache const& cache, bool fullscreen, bool liveOnly, int overworldSkyDarken);
    void acceptImage(MapView const& texture, MapLayer layer, std::uint64_t revision,
        std::vector<std::uint32_t> pixels, BiomeMap biomes, bool biomeRegions, MapLighting lighting);
    void presentView(MapView const& visible, bool fullscreen);
    MapView const& displayedView() const { return renderedView; }
    void invalidate() { pixels.clear(); lastRaster = {}; ++generation; }
    void retryImage() { lastRaster = {}; }
    void resetResources() { resetTexture = true; }
    void beginFrame();
    bool draw(MinecraftUIRenderContext& context, MapFrame const& frame);
    void shutdown();

private:
    std::unique_ptr<MapRenderer> renderer;
    bool resetTexture{};
    MapView renderedView, textureView, rasterTarget;
    std::vector<std::uint32_t> pixels;
    BiomeMap biomes;
    bool rasterBiomes{};
    MapLighting rasterLighting;
    std::uint64_t imageRevision{}, rasterRevision{};
    MapLayer rasterLayer{std::numeric_limits<int>::min()};
    Clock::time_point lastRaster{};
    struct LiveImage {
        MapView view;
        MapLayer layer;
        MapLighting lighting;
        bool biomeRegions{};
        std::uint64_t revision{}, generation{};
        std::vector<std::uint32_t> pixels;
        BiomeMap biomes;
    };
    std::uint64_t generation{};
    std::shared_ptr<MapRasterBuffer> liveBuffer;
    std::future<LiveImage> liveTask;
};
} // namespace wayfinder
