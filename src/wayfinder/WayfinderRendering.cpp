#include "wayfinder/WayfinderInternal.h"
#include "ll/api/mod/NativeMod.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/GuiData.h"

namespace wayfinder {
void Wayfinder::Impl::Rendering::refreshImage(Impl& app, MapView const& view) {
    bool smooth = !app.view.fullscreen;
    auto request = smooth ? minimapTextureView(view) : view;
    if (app.session.history.ready()) app.session.history.finish(app);
    bool urgent = pixels.empty() || textureView.width != request.width || textureView.height != request.height ||
                  textureView.blocksPerPixel != request.blocksPerPixel ||
                  rasterLayer != app.session.layer ||
                  (smooth && !textureCoversView(textureView, view, 1));
    bool liveOnly = !app.session.history.archive;
    if (urgent || (liveOnly && rasterRevision != app.session.cache.revision() &&
                   Clock::now() - lastRaster >= std::chrono::milliseconds(app.settings.refreshMilliseconds))) {
        // Teleports or a slow worker may exhaust the border. Rebuild locally
        // before drawing instead of clamping UVs and stretching an edge texel.
        pixels = app.session.cache.rasterize(app.session.layer, request);
        textureView = request;
        rasterLayer = app.session.layer;
        rasterRevision = liveOnly ? app.session.cache.revision() : std::numeric_limits<std::uint64_t>::max();
        ++imageRevision;
        if (liveOnly && !urgent) lastRaster = Clock::now();
    }
    if (urgent || (Clock::now() - lastRaster >= std::chrono::milliseconds(app.settings.refreshMilliseconds) &&
                   (textureView != request || rasterRevision != app.session.cache.revision()))) {
        app.session.history.requestedView = request;
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
    refreshImage(app, view);
    if (!renderer) renderer = std::make_unique<MapRenderer>();
    auto layerLabel = dimensionName(app.session.player.dimension, app.ui.state.locale);
    if (app.session.layer.underground())
        layerLabel += fmt::format(" | {} Y {}", app.ui.state.locale.tr("terrain.cave"),
            app.session.layer.referenceY());
    std::string caption;
    if (app.settings.showCoordinates)
        caption = fmt::format("X {}  Y {}  Z {}", blockCoordinate(app.session.player.x),
            blockCoordinate(app.session.player.y), blockCoordinate(app.session.player.z));
    auto historyStatus = app.session.history.status(app.ui.state.locale);
    if (!historyStatus.empty()) caption = historyStatus + (caption.empty() ? "" : " | " + caption);
    if (app.view.fullscreen && app.settings.showCoordinates && app.ui.state.page == MapMenu::Page::Map &&
        app.input.hasMouse && app.view.area.contains(app.input.mouseX, app.input.mouseY)) {
        auto point =
            renderedView.worldAt((app.input.mouseX - app.view.area.x) / app.view.area.width * renderedView.width,
                                 (app.input.mouseY - app.view.area.y) / app.view.area.height * renderedView.height);
        caption += fmt::format("  |  {} X {}  Z {}", app.ui.state.locale.tr("Cursor"), blockCoordinate(point[0]),
                               blockCoordinate(point[1]));
    }
    try {
        if (app.view.fullscreen) {
            ctx.saveCurrentClippingRectangle();
            ctx.setFullClippingRectangle();
            ctx.fillRectangle(RectangleArea{0, width, 0, height}, mce::Color(10, 16, 23), 0.97f);
            ctx.restoreSavedClippingRectangle();
        }
        renderer->render(ctx, app.view.area, renderedView, textureView, pixels, imageRevision, app.session.player.x,
                         app.session.player.z, app.session.player.yaw, caption, app.view.fullscreen, app.settings,
                         app.ui.state.locale, app.view.fullscreen ? app.view.following : true,
                         app.session.player.biome, layerLabel);
        renderer->renderMarkers(ctx, app.view.area, renderedView, app.session.navigation, app.session.player.dimension,
                                app.session.player.x, app.session.player.z, app.view.fullscreen, app.settings,
                                app.ui.state.locale);
        renderer->renderEntities(ctx, app.view.area, renderedView, app.session.entities, app.session.layer,
                                 app.session.player.x, app.session.player.y, app.session.player.z,
                                 app.settings, app.ui.state.locale, app.view.fullscreen);
        if (app.view.fullscreen) {
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
