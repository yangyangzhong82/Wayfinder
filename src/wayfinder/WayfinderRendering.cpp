#include "wayfinder/WayfinderRendering.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"

namespace wayfinder {
std::optional<MapView> WayfinderRendering::refreshImage(MapView const& view, MapLayer layer,
    Settings const& settings, MapCache const& cache, bool fullscreen, bool liveOnly, int overworldSkyDarken) {
    bool smooth = !fullscreen;
    auto request = detailTextureView(smooth ? minimapTextureView(view) : view, settings.terrainPixelScale);
    MapLighting lighting{settings.showLighting, layer.dimension == 0 ? overworldSkyDarken : 0};
    if (liveTask.valid() && liveTask.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto result = liveTask.get();
        if (liveOnly && result.generation == generation && result.layer == layer && result.lighting == lighting
            && result.biomeRegions == settings.showBiomeRegions && result.view.width == request.width
            && result.view.height == request.height && result.view.blocksPerPixel == request.blocksPerPixel
            && (fullscreen || textureCoversView(result.view, view, settings.terrainPixelScale)))
            acceptImage(result.view, result.layer, result.revision, std::move(result.pixels), std::move(result.biomes),
                        result.biomeRegions, result.lighting);
    }
    if (!liveOnly && !liveTask.valid()) liveBuffer.reset();
    bool urgent = pixels.empty() || rasterTarget.width != request.width || rasterTarget.height != request.height ||
                  rasterTarget.blocksPerPixel != request.blocksPerPixel ||
                  rasterLayer != layer || rasterBiomes != settings.showBiomeRegions ||
                  (smooth && !textureCoversView(textureView, view, 1));
    if (urgent) {
        // Keep urgent UI work bounded even while history is opening or its worker is busy.
        // The preview has the same world coverage; detailed material/biome work stays off-thread.
        textureView = previewTextureView(request);
        cache.preview(layer, textureView, lighting, pixels);
        rasterLighting = lighting;
        biomes = {};
        rasterBiomes = settings.showBiomeRegions;
        rasterTarget = request;
        rasterLayer = layer;
        rasterRevision = std::numeric_limits<std::uint64_t>::max();
        ++imageRevision;
    }
    if (urgent || (Clock::now() - lastRaster >= std::chrono::milliseconds(settings.refreshMilliseconds) &&
                   (textureView != request || rasterRevision != cache.revision() || rasterLighting != lighting))) {
        if (liveOnly) {
            // Never replace an unfinished future: destroying it would wait on the UI thread.
            if (liveTask.valid()) return {};
            if (!liveBuffer) liveBuffer = std::make_shared<MapRasterBuffer>();
            LiveImage result{request, layer, lighting, settings.showBiomeRegions, cache.revision(), generation, {}, {}};
            liveTask = std::async(std::launch::async,
                [snapshot = cache.snapshotForView(layer, request), buffer = liveBuffer, result = std::move(result)]() mutable {
                    // Destroy tile snapshots on the worker, including when rasterization throws.
                    auto local = std::move(snapshot);
                    result.pixels = local.rasterize(result.layer, result.view, result.lighting, buffer.get());
                    if (result.biomeRegions) result.biomes = rasterizeBiomes(local, result.layer, result.view);
                    return std::move(result);
                });
            lastRaster = Clock::now();
            return {};
        }
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
    rasterTarget = texture;
    rasterLayer = layer;
    rasterRevision = revision;
    rasterBiomes = biomeRegions;
    rasterLighting = lighting;
    ++imageRevision;
}
void WayfinderRendering::presentView(MapView const& visible, bool fullscreen) {
    renderedView = visible;
    if (fullscreen) {
        // Texture dimensions may include rounding padding from the pixel limit.
        // Keep logical projection/zoom unchanged and crop the padding using UVs.
        renderedView.centerX = textureView.centerX;
        renderedView.centerZ = textureView.centerZ;
    }
}
void WayfinderRendering::beginFrame() {
    if (!resetTexture) return;
    if (renderer) renderer->reset();
    invalidate();
    biomes = {};
    resetTexture = false;
}
void WayfinderRendering::shutdown() {
    // Only shutdown waits. Workers own their snapshots and never touch renderer/engine state.
    if (liveTask.valid()) {
        liveTask.wait();
        liveTask = {};
    }
    liveBuffer.reset();
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
