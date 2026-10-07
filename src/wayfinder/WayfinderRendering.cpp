#include "wayfinder/WayfinderRendering.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"

namespace wayfinder {
std::optional<MapView> WayfinderRendering::refreshImage(MapView const& view, MapLayer layer,
    Settings const& settings, MapCache const& cache, bool fullscreen, bool liveOnly, int overworldSkyDarken) {
    bool smooth = !fullscreen;
    auto request = detailTextureView(smooth ? minimapTextureView(view) : view, settings.terrainPixelScale);
    MapLighting lighting{settings.showLighting, layer.dimension == 0 ? overworldSkyDarken : 0};
    bool urgent = pixels.empty() || textureView.width != request.width || textureView.height != request.height ||
                  textureView.blocksPerPixel != request.blocksPerPixel ||
                  rasterLayer != layer || rasterBiomes != settings.showBiomeRegions ||
                  (smooth && !textureCoversView(textureView, view, settings.terrainPixelScale));
    if (urgent || (liveOnly && (rasterRevision != cache.revision() || rasterLighting != lighting) &&
                   Clock::now() - lastRaster >= std::chrono::milliseconds(settings.refreshMilliseconds))) {
        // Teleports or a slow worker may exhaust the border. Rebuild locally
        // before drawing instead of clamping UVs and stretching an edge texel.
        pixels = cache.rasterize(layer, request, lighting);
        rasterLighting = lighting;
        biomes = settings.showBiomeRegions ? rasterizeBiomes(cache, layer, request) : BiomeMap{};
        rasterBiomes = settings.showBiomeRegions;
        textureView = request;
        rasterLayer = layer;
        rasterRevision = liveOnly ? cache.revision() : std::numeric_limits<std::uint64_t>::max();
        ++imageRevision;
        if (liveOnly && !urgent) lastRaster = Clock::now();
    }
    if (urgent || (Clock::now() - lastRaster >= std::chrono::milliseconds(settings.refreshMilliseconds) &&
                   (textureView != request || rasterRevision != cache.revision() || rasterLighting != lighting))) {
        lastRaster = Clock::now();
        return request;
    }
    return {};
}

void WayfinderRendering::acceptImage(MapView const& texture, MapLayer layer, std::uint64_t revision,
    std::vector<std::uint32_t> image, BiomeMap regions, bool biomeRegions, MapLighting lighting) {
    pixels = std::move(image);
    biomes = std::move(regions);
    textureView = texture;
    rasterLayer = layer;
    rasterRevision = revision;
    rasterBiomes = biomeRegions;
    rasterLighting = lighting;
    ++imageRevision;
}
void WayfinderRendering::presentView(MapView const& visible, bool fullscreen) {
    renderedView = fullscreen ? textureView : visible;
}
void WayfinderRendering::beginFrame() {
    if (!resetTexture) return;
    if (renderer) renderer->reset();
    invalidate();
    biomes = {};
    resetTexture = false;
}
void WayfinderRendering::shutdown() {
    renderer.reset();
    invalidate();
    biomes = {};
}
bool WayfinderRendering::draw(MinecraftUIRenderContext& ctx, MapFrame const& frame) {
    if (!renderer) renderer = std::make_unique<MapRenderer>();
    if (frame.fullscreen) {
        ctx.saveCurrentClippingRectangle();
        struct RestoreClip {
            MinecraftUIRenderContext& context;
            ~RestoreClip() { context.restoreSavedClippingRectangle(); }
        } restoreClip{ctx};
        ctx.setFullClippingRectangle();
        ctx.fillRectangle(RectangleArea{0, frame.guiWidth, 0, frame.guiHeight}, mce::Color(10, 16, 23), 0.97f);
    }
    bool terrainReady = renderer->render(ctx, frame.area, renderedView, textureView, pixels, biomes, imageRevision, frame.playerX,
                     frame.playerZ, frame.yaw, frame.caption, frame.fullscreen, frame.settings,
                     frame.locale, frame.fullscreen ? frame.following : true,
                     frame.biome, frame.cursorInfo, frame.layerLabel, frame.trail, frame.layer, frame.layer == frame.liveLayer);
    Navigation retraceNavigation;
    auto navigation = &frame.navigation;
    std::optional<Settings> retraceSettings;
    if (auto target = frame.trail.retraceTarget(frame.locale)) {
        auto const& trailPoint = frame.trail.points[*frame.trail.retraceIndex()];
        if (trailPoint.layer != frame.liveLayer && target->y && std::abs(*target->y - frame.playerY) >= 0.5)
            target->name = frame.locale.tr(target->y && *target->y > frame.playerY
                ? "Retrace upstairs" : "Retrace downstairs");
        retraceNavigation = *navigation;
        target->id = retraceNavigation.nextId;
        target->color = 5;
        retraceNavigation.points.push_back(*target);
        retraceNavigation.target = target->id;
        navigation = &retraceNavigation;
        if (!frame.settings.showNavigation) {
            retraceSettings = frame.settings;
            retraceSettings->showNavigation = true;
        }
    }
    renderer->renderMarkers(ctx, frame.area, renderedView, *navigation, frame.layer, frame.playerDimension,
                            frame.playerX, frame.playerY, frame.playerZ, frame.fullscreen,
                            retraceSettings ? *retraceSettings : frame.settings,
                            frame.locale);
    if (frame.layer == frame.liveLayer) renderer->renderEntities(ctx, frame.area, renderedView, frame.entities, frame.layer,
                             frame.playerX, frame.playerY, frame.playerZ,
                             frame.settings, frame.locale, frame.fullscreen);
    if (frame.ui) renderer->renderUi(ctx, *frame.ui, frame.mouseX, frame.mouseY);
    return terrainReady;
}
} // namespace wayfinder
