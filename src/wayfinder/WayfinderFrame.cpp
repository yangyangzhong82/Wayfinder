#include "wayfinder/WayfinderInternal.h"
#include "ll/api/mod/NativeMod.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/GuiData.h"

namespace wayfinder {
void Wayfinder::Impl::notifyRender(std::string_view message) {
    // Use the game's local chat UI, independent of the failed map textures.
    // No packet is sent and no raw exception text is shown to the player.
    try {
        if (auto client = session.client)
            client->getGuiData()->displayClientMessage("[Wayfinder] " + ui.state.locale.tr(message), std::nullopt, false);
    } catch (std::exception const& ex) {
        mod.getLogger().warn("Wayfinder render notification: {}", ex.what());
    }
}

void Wayfinder::Impl::render(ll::event::AfterUIRenderEvent& event) {
    if (!event.uiRenderContext().mClient.isPrimaryClient() || !renderRecovery.ready(Clock::now())) return;
    try {
        if (drawFrame(event) && renderRecovery.succeeded(Clock::now())) notifyRender("render.restored");
    } catch (std::exception const& ex) {
        renderRecovery.failed(Clock::now());
        rendering.resetResources();
        // Restore input immediately. Rebuilding resources is deferred to a later
        // UI callback, never performed while unwinding the failed draw.
        try { closeMap(true); }
        catch (std::exception const& cleanup) {
            mod.getLogger().warn("Wayfinder render input cleanup: {}", cleanup.what());
        }
        mod.getLogger().error("Wayfinder render failure {} ({}): {}", renderRecovery.failures(),
            renderRecovery.stopped() ? "automatic retries stopped" : "will retry", ex.what());
        if (renderRecovery.stopped()) notifyRender("render.stopped");
        else if (renderRecovery.failures() == 1) notifyRender("render.retrying");
    }
}

bool Wayfinder::Impl::drawFrame(ll::event::AfterUIRenderEvent& event) {
    auto& ctx = event.uiRenderContext();
    auto& ci = ctx.mClient;
    if (!ci.isPrimaryClient()) return false;
    rendering.beginFrame();
    if (!session.client || session.client != &ci || !ci.getLocalPlayer()) return false;
    pollText();
    // Keep the map alive while its embedded text editor is completing.
    if (view.fullscreen && !ui.textInput.active() && (!foreground() || !hud(ci)))
        closeMap(false);
    if (event.screenView().getScreenName() != "hud_screen" || !hud(ci)) return false;
    if (!view.fullscreen && !settings.showMinimap) return false;
    ui.refreshLocale(settings);
    auto gui = ci.getGuiData();
    auto const& size = gui->mScreenSizeData;
    float width = size->clientUIScreenSize->x, height = size->clientUIScreenSize->y;
    if (width < 112 || height < 144) return false;
    if (view.fullscreen && (view.guiWidth != width || view.guiHeight != height)) {
        input.dragging = false;
        input.hasMouse = false;
    }
    view.guiWidth = width;
    view.guiHeight = height;
    if (view.fullscreen) {
        if (ui.state.page == MapMenu::Page::Map) input.panHeldKeys(view);
        else input.lastPan = Clock::now();
        input.pollPointer(view, ui.state.page == MapMenu::Page::Map);
    }
    auto mapView = view.layout(width, height, settings, session.player.x, session.player.z);
    auto layer = view.displayedLayer(session.layer);
    collectHistory();
    if (auto request = rendering.refreshImage(mapView, layer, settings, session.cache,
            view.fullscreen, session.history.liveOnly(), session.overworldSkyDarken)) {
        session.history.requestView(*request, layer);
        saveHistory();
    }
    rendering.presentView(mapView, view.fullscreen);
    auto layerLabel = dimensionName(layer.dimension, ui.state.locale);
    if (layer.underground())
        layerLabel += fmt::format(" | {} Y {}", ui.state.locale.tr("terrain.cave"),
            layer.referenceY());
    if (view.fullscreen && view.lockedLayer) layerLabel += " | " + ui.state.locale.tr("Layer locked");
    std::string caption;
    if (settings.showCoordinates)
        caption = fmt::format("X {}  Y {}  Z {}", blockCoordinate(session.player.x),
            blockCoordinate(session.player.y), blockCoordinate(session.player.z));
    auto historyStatus = session.history.status(ui.state.locale);
    if (!historyStatus.empty()) caption = historyStatus + (caption.empty() ? "" : " | " + caption);
    std::optional<MapInspectionPoint> cursorPoint;
    if (view.fullscreen && ui.state.page == MapMenu::Page::Map && input.hasMouse)
        cursorPoint = mapInspectionPoint(rendering.displayedView(), view.area, layer, input.mouseX, input.mouseY);
    session.cursorBiome.select(settings.showBiome ? cursorPoint : std::nullopt);
    std::string cursorInfo;
    if (cursorPoint) {
        if (settings.showCoordinates)
            cursorInfo = fmt::format("{} X {}  Z {}", ui.state.locale.tr("Cursor"), cursorPoint->x, cursorPoint->z);
        if (settings.showBiome) {
            if (!cursorInfo.empty()) cursorInfo += "  |  ";
            auto const& biome = session.cursorBiome.identifier;
            cursorInfo += ui.state.locale.tr("Cursor biome: ")
                + (biome.empty() ? ui.state.locale.tr("Unknown") : ui.state.locale.biome(biome));
        }
    }
    if (view.fullscreen) {
        if (ui.state.page == MapMenu::Page::Explore) refreshExploration();
        ui.frame = ui.state.build(width, height, settings, session.navigation);
    }
    MapFrame frame{settings, ui.state.locale, session.navigation, session.trail, session.entities};
    frame.ui = view.fullscreen ? &ui.frame : nullptr;
    frame.area = view.area;
    frame.layer = layer;
    frame.liveLayer = session.layer;
    frame.playerX = session.player.x;
    frame.playerY = session.player.y;
    frame.playerZ = session.player.z;
    frame.playerDimension = session.player.dimension;
    frame.yaw = session.player.yaw;
    frame.guiWidth = width;
    frame.guiHeight = height;
    frame.mouseX = input.mouseX;
    frame.mouseY = input.mouseY;
    frame.fullscreen = view.fullscreen;
    frame.following = !view.fullscreen || view.following;
    frame.caption = std::move(caption);
    frame.biome = session.player.biome;
    frame.cursorInfo = std::move(cursorInfo);
    frame.layerLabel = std::move(layerLabel);
    return rendering.draw(ctx, frame);
}
} // namespace wayfinder
