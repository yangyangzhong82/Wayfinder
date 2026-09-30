#include "wayfinder/WayfinderInternal.h"
#include "ll/api/mod/NativeMod.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/GuiData.h"

namespace wayfinder {
void Wayfinder::Impl::Rendering::refreshImage(Impl& app, MapView const& view) {
    bool smooth = !app.view.fullscreen;
    auto request = detailTextureView(smooth ? minimapTextureView(view) : view, app.settings.terrainPixelScale);
    auto layer = app.view.displayedLayer(app.session.layer);
    MapLighting lighting{app.settings.showLighting, layer.dimension == 0 ? app.session.overworldSkyDarken : 0};
    if (app.session.history.ready()) app.session.history.finish(app);
    bool urgent = pixels.empty() || textureView.width != request.width || textureView.height != request.height ||
                  textureView.blocksPerPixel != request.blocksPerPixel ||
                  rasterLayer != layer || rasterBiomes != app.settings.showBiomeRegions ||
                  (smooth && !textureCoversView(textureView, view, app.settings.terrainPixelScale));
    bool liveOnly = !app.session.history.archive;
    if (urgent || (liveOnly && (rasterRevision != app.session.cache.revision() || rasterLighting != lighting) &&
                   Clock::now() - lastRaster >= std::chrono::milliseconds(app.settings.refreshMilliseconds))) {
        // Teleports or a slow worker may exhaust the border. Rebuild locally
        // before drawing instead of clamping UVs and stretching an edge texel.
        pixels = app.session.cache.rasterize(layer, request, lighting);
        rasterLighting = lighting;
        biomes = app.settings.showBiomeRegions ? rasterizeBiomes(app.session.cache, layer, request) : BiomeMap{};
        rasterBiomes = app.settings.showBiomeRegions;
        textureView = request;
        rasterLayer = layer;
        rasterRevision = liveOnly ? app.session.cache.revision() : std::numeric_limits<std::uint64_t>::max();
        ++imageRevision;
        if (liveOnly && !urgent) lastRaster = Clock::now();
    }
    if (urgent || (Clock::now() - lastRaster >= std::chrono::milliseconds(app.settings.refreshMilliseconds) &&
                   (textureView != request || rasterRevision != app.session.cache.revision() || rasterLighting != lighting))) {
        app.session.history.requestedView = request;
        app.session.history.requestedLayer = layer;
        lastRaster = Clock::now();
        app.session.history.save(app, false);
    }
    // Only UVs and overlays move each frame; texture uploads still follow imageRevision.
    renderedView = smooth ? view : textureView;
}

void Wayfinder::Impl::Rendering::render(Impl& app, ll::event::AfterUIRenderEvent& event) {
    auto& ctx = event.uiRenderContext();
    auto& ci = ctx.mClient;
    if (!ci.isPrimaryClient()) return;
    if (resetTexture) {
        if (renderer) renderer->reset();
        resetTexture = false;
    }
    if (!app.session.client || app.session.client != &ci || !ci.getLocalPlayer()) return;
    app.ui.pollText(app);
    if (app.view.fullscreen && (!foreground() || !hud(ci))) app.input.closeMap(app, false);
    if (failed || event.screenView().getScreenName() != "hud_screen" || !hud(ci)) return;
    if (!app.view.fullscreen && !app.settings.showMinimap) return;
    app.ui.refreshLocale(app.settings);
    auto gui = ci.getGuiData();
    auto const& size = gui->mScreenSizeData;
    float width = size->clientUIScreenSize->x, height = size->clientUIScreenSize->y;
    if (width < 112 || height < 144) return;
    if (app.view.fullscreen && (app.view.guiWidth != width || app.view.guiHeight != height)) {
        app.input.dragging = false;
        app.input.hasMouse = false;
    }
    app.view.guiWidth = width;
    app.view.guiHeight = height;
    if (app.view.fullscreen) {
        if (app.ui.state.page == MapMenu::Page::Map) app.input.panHeldKeys(app);
        else app.input.lastPan = Clock::now();
        app.input.pollPointer(app);
    }
    auto view = app.view.layout(width, height, app.settings, app.session.player.x, app.session.player.z);
    auto layer = app.view.displayedLayer(app.session.layer);
    refreshImage(app, view);
    if (!renderer) renderer = std::make_unique<MapRenderer>();
    auto layerLabel = dimensionName(layer.dimension, app.ui.state.locale);
    if (layer.underground())
        layerLabel += fmt::format(" | {} Y {}", app.ui.state.locale.tr("terrain.cave"),
            layer.referenceY());
    if (app.view.fullscreen && app.view.lockedLayer) layerLabel += " | " + app.ui.state.locale.tr("Layer locked");
    std::string caption;
    if (app.settings.showCoordinates)
        caption = fmt::format("X {}  Y {}  Z {}", blockCoordinate(app.session.player.x),
            blockCoordinate(app.session.player.y), blockCoordinate(app.session.player.z));
    auto historyStatus = app.session.history.status(app.ui.state.locale);
    if (!historyStatus.empty()) caption = historyStatus + (caption.empty() ? "" : " | " + caption);
    std::optional<MapInspectionPoint> cursorPoint;
    if (app.view.fullscreen && app.ui.state.page == MapMenu::Page::Map && app.input.hasMouse)
        cursorPoint = mapInspectionPoint(renderedView, app.view.area, layer, app.input.mouseX, app.input.mouseY);
    app.session.cursorBiome.select(app.settings.showBiome ? cursorPoint : std::nullopt);
    std::string cursorInfo;
    if (cursorPoint) {
        if (app.settings.showCoordinates)
            cursorInfo = fmt::format("{} X {}  Z {}", app.ui.state.locale.tr("Cursor"), cursorPoint->x, cursorPoint->z);
        if (app.settings.showBiome) {
            if (!cursorInfo.empty()) cursorInfo += "  |  ";
            auto const& biome = app.session.cursorBiome.identifier;
            cursorInfo += app.ui.state.locale.tr("Cursor biome: ")
                + (biome.empty() ? app.ui.state.locale.tr("Unknown") : app.ui.state.locale.biome(biome));
        }
    }
    try {
        if (app.view.fullscreen) {
            ctx.saveCurrentClippingRectangle();
            ctx.setFullClippingRectangle();
            ctx.fillRectangle(RectangleArea{0, width, 0, height}, mce::Color(10, 16, 23), 0.97f);
            ctx.restoreSavedClippingRectangle();
        }
        renderer->render(ctx, app.view.area, renderedView, textureView, pixels, biomes, imageRevision, app.session.player.x,
                         app.session.player.z, app.session.player.yaw, caption, app.view.fullscreen, app.settings,
                         app.ui.state.locale, app.view.fullscreen ? app.view.following : true,
                         app.session.player.biome, cursorInfo, layerLabel, app.session.trail, layer, layer == app.session.layer);
        Navigation retraceNavigation;
        auto navigation = &app.session.navigation;
        std::optional<Settings> retraceSettings;
        if (auto target = app.session.trail.retraceTarget(app.ui.state.locale)) {
            auto const& trailPoint = app.session.trail.points[*app.session.trail.retraceIndex()];
            if (trailPoint.layer != app.session.layer && target->y && std::abs(*target->y - app.session.player.y) >= 0.5)
                target->name = app.ui.state.locale.tr(target->y && *target->y > app.session.player.y
                    ? "Retrace upstairs" : "Retrace downstairs");
            retraceNavigation = *navigation;
            target->id = retraceNavigation.nextId;
            target->color = 5;
            retraceNavigation.points.push_back(*target);
            retraceNavigation.target = target->id;
            navigation = &retraceNavigation;
            if (!app.settings.showNavigation) {
                retraceSettings = app.settings;
                retraceSettings->showNavigation = true;
            }
        }
        renderer->renderMarkers(ctx, app.view.area, renderedView, *navigation, layer, app.session.player.dimension,
                                app.session.player.x, app.session.player.y, app.session.player.z, app.view.fullscreen,
                                retraceSettings ? *retraceSettings : app.settings,
                                app.ui.state.locale);
        if (layer == app.session.layer) renderer->renderEntities(ctx, app.view.area, renderedView, app.session.entities, layer,
                                 app.session.player.x, app.session.player.y, app.session.player.z,
                                 app.settings, app.ui.state.locale, app.view.fullscreen);
        if (app.view.fullscreen) {
            if (app.ui.state.page == MapMenu::Page::Explore) app.ui.refreshExploration(app);
            app.ui.frame = app.ui.state.build(width, height, app.settings, app.session.navigation);
            renderer->renderUi(ctx, app.ui.frame, app.input.mouseX, app.input.mouseY);
        }
    } catch (std::exception const& ex) {
        failed = true;
        app.input.closeMap(app, true);
        app.mod.getLogger().error("Wayfinder renderer disabled for this session: {}", ex.what());
    }
}
} // namespace wayfinder
