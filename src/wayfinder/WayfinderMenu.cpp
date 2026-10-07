#include "wayfinder/WayfinderInternal.h"
#include "wayfinder/MapOverlays.h"
#include "wayfinder/MapTeleport.h"
#include "ll/api/mod/NativeMod.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/locale/I18n.h"
#include "mc/locale/Localization.h"
#include "mc/network/packet/CommandRequestPacket.h"
#include "mc/server/commands/CommandContext.h"
#include "mc/server/commands/PlayerCommandOrigin.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/chunk/LevelChunk.h"

namespace wayfinder {
void Wayfinder::Impl::Menu::refreshLocale(Settings const& settings) {
    // Refresh resource-pack/game language using the exposed mCode field.
    gameLanguage = *getI18n().getCurrentLanguage()->mCode;
    state.locale.select(settings.language, gameLanguage);
}

void Wayfinder::Impl::newWaypoint(bool atPointer) {
    Waypoint point;
    point.name = ui.state.locale.tr("Waypoint");
    point.dimension = session.player.dimension;
    point.x = blockCoordinate(session.player.x);
    point.y = blockCoordinate(session.player.y);
    point.z = blockCoordinate(session.player.z);
    point.created = unixTime();
    if (atPointer && input.hasMouse && view.area.contains(input.mouseX, input.mouseY)) {
        point.dimension = view.displayedLayer(session.layer).dimension;
        auto world = rendering.displayedView().worldAt(
            (input.mouseX - view.area.x) / view.area.width * rendering.displayedView().width,
            (input.mouseY - view.area.y) / view.area.height * rendering.displayedView().height);
        point.x = blockCoordinate(world[0]);
        point.z = blockCoordinate(world[1]);
        auto cell = session.cache.get(view.displayedLayer(session.layer), point.x, point.z);
        point.y = cell.floor() ? std::optional<int>(cell.height) : std::nullopt;
    }
    ui.state.edit(std::move(point));
    input.panKeys.clear();
    input.dragging = false;
}

void Wayfinder::Impl::menuAction(UiButton const& button) {
    input.panKeys.clear();
    input.dragging = false;
    ui.refreshLocale(settings);
    if (!button.enabled) return;
    if (button.action != UiAction::ClearTrail) ui.state.confirmClearTrail = false;
    if (button.action == UiAction::Explore) refreshExploration();
    if (button.action == UiAction::LiveLayer) {
        followPlayer();
        return;
    }
    if (button.action == UiAction::SelectLayer && ui.state.page == MapMenu::Page::Explore) {
        if (button.id >= ui.state.knownLayers.size()) return;
        selectDisplayedLayer(ui.state.knownLayers[static_cast<std::size_t>(button.id)]);
        return;
    }
    if (button.action == UiAction::Retrace && ui.state.page == MapMenu::Page::Explore) {
        if (session.trail.retraceIndex()) session.trail.stopRetrace();
        else if (!session.trail.startRetrace(session.layer, session.player.x,
                     session.player.y, session.player.z)) {
            ui.state.fail("No nearby continuous trail in this layer.");
            return;
        }
        followPlayer();
        return;
    }
    if (button.action == UiAction::ClearTrail && ui.state.page == MapMenu::Page::Explore) {
        if (!ui.state.confirmClearTrail) { ui.state.confirmClearTrail = true; return; }
        session.trail.clear();
        persistTrail(true);
        ui.state.confirmClearTrail = false;
        ui.state.message = session.trailSaveFailed ? "Trail save failed; retrying" : "Trail cleared";
        refreshExploration();
        return;
    }
    if (button.action == UiAction::NewPlayer) {
        newWaypoint(false);
        return;
    }
    if (button.action == UiAction::Follow) {
        followPlayer();
        return;
    }
    if (button.action == UiAction::Locate) {
        ui.state.open(MapMenu::Page::Locate);
        ui.state.draft = {};
        ui.state.draft.name = ui.state.locale.tr("Waypoint");
        ui.state.draft.dimension = view.displayedLayer(session.layer).dimension;
        ui.state.draft.x = blockCoordinate(session.player.x);
        ui.state.draft.z = blockCoordinate(session.player.z);
        ui.state.draft.created = unixTime();
        return;
    }
    auto previousMenu = ui.state;
    try {
        if (button.action == UiAction::Teleport) {
            if (ui.state.page == MapMenu::Page::Context && button.enabled) teleport();
            return;
        }
        auto changedSettings = settings;
        auto changedNavigation = session.navigation;
        int change = ui.state.action(button, changedSettings, changedNavigation);
        if (change == 1) {
            writeNavigation(session.navigationPath, session.identity, changedNavigation);
            session.navigation = std::move(changedNavigation);
            session.navigationDirty = false;
            if (button.action == UiAction::Navigate || button.action == UiAction::NavigateExisting || button.action == UiAction::Stop)
                session.trail.stopRetrace();
        } else if (change == 2) {
            saveSettings(mod.getConfigDir(), changedSettings);
            bool resample = settings.includeWater != changedSettings.includeWater
                || settings.terrainMode != changedSettings.terrainMode
                || settings.caveSwitchY != changedSettings.caveSwitchY
                || settings.includeLeaves != changedSettings.includeLeaves
                || settings.sampleRadiusChunks != changedSettings.sampleRadiusChunks;
            bool resize = settings.minimapPixels != changedSettings.minimapPixels
                || settings.fullscreenPixels != changedSettings.fullscreenPixels
                || settings.terrainPixelScale != changedSettings.terrainPixelScale
                || settings.minimapBlocksPerPixel != changedSettings.minimapBlocksPerPixel;
            settings = changedSettings;
            if (!settings.recordTrail) session.trail.breakSegment();
            session.lastEntities = {};
            if (!settings.showEntities) session.entities.clear();
            if (resample) session.sampler.reset();
            if (resize) rendering.invalidate();
            ui.state.locale.select(settings.language, ui.gameLanguage);
        } else if (change == 3 || change == 4) {
            auto point = ui.state.draft;
            locateView(view.fullView, point.x, point.z);
            view.following = false;
            invalidateDisplayedMap();
            if (change == 4) {
                auto cell = session.cache.get(view.displayedLayer(session.layer), point.x, point.z);
                if (!point.y) point.y = cell.floor() ? std::optional<int>(cell.height) : std::nullopt;
                ui.state.edit(std::move(point));
            } else ui.state.open(MapMenu::Page::Map);
        }
    } catch (std::exception const& ex) {
        ui.state = std::move(previousMenu);
        ui.state.fail(ex.what());
        mod.getLogger().warn("Wayfinder UI: {}", ex.what());
    }
    ui.frame = ui.state.build(view.guiWidth, view.guiHeight, settings, session.navigation);
    if (ui.state.textField()) openText();
}

void Wayfinder::Impl::openText() {
    if (!ui.state.textField() || ui.textInput.active() || !view.fullscreen) return;
    // Actions run on mouse-down. Let the opening click finish before activating
    // another window, otherwise its mouse-up can return focus to the game.
    if (GetForegroundWindow() != input.mapWindow || (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return;
    NativeTextInput::Request request;
    char const* label = ui.state.focus == UiAction::Name ? "Name"
        : ui.state.focus == UiAction::Group || ui.state.focus == UiAction::BatchGroup ? "Group"
        : ui.state.focus == UiAction::Search || ui.state.focus == UiAction::EntitySearch ? "Search"
        : "Coordinates";
    request.title = "Wayfinder - " + ui.state.locale.tr(label);
    request.text = ui.state.text;
    bool coordinates = ui.state.focus == UiAction::CoordinateText || ui.state.focus == UiAction::X
        || ui.state.focus == UiAction::Y || ui.state.focus == UiAction::Z;
    request.hint = ui.state.errorMessage ? ui.state.locale.error(ui.state.message)
        : ui.state.locale.tr(coordinates ? "Coordinate input hint" : "Native text input hint");
    request.accept = ui.state.locale.tr("Done");
    request.cancel = ui.state.locale.tr("Cancel");
    GetWindowRect(input.mapWindow, &request.anchor);
    request.owner = input.mapWindow;
    try {
        ui.textInput.start(std::move(request));
        ui.nativeField = ui.state.focus;
        ui.nativePage = ui.state.page;
    } catch (std::exception const& ex) {
        ui.state.cancelField();
        ui.state.fail("Unable to open text editor");
        mod.getLogger().warn("Wayfinder text input: {}", ex.what());
    }
}

void Wayfinder::Impl::pollText() {
    if (!ui.textInput.active()) openText();
    auto result = ui.textInput.take();
    if (!result) return;
    auto field = ui.nativeField;
    ui.nativeField = UiAction::None;
    if (!view.fullscreen || field == UiAction::None || ui.state.focus != field || ui.state.page != ui.nativePage) return;
    if (!result->error.empty()) {
        ui.state.cancelField();
        ui.state.fail("Unable to open text editor");
        mod.getLogger().warn("Wayfinder text input: {}", result->error);
        return;
    }
    if (!result->accepted) ui.state.cancelField();
    else {
        ui.state.text = std::move(result->text);
        try {
            ui.state.commitField();
            ui.state.message.clear();
            ui.state.errorMessage = false;
        } catch (std::exception const& ex) {
            ui.state.fail(ex.what());
            openText();
        }
    }
    ui.frame = ui.state.build(view.guiWidth, view.guiHeight, settings, session.navigation);
}

void Wayfinder::Impl::openContext() {
    if (!input.hasMouse || !view.area.contains(input.mouseX, input.mouseY)) return;
    ui.refreshLocale(settings);
    // Resolve once, before the pointer moves onto a menu action.
    newWaypoint(true);
    ui.state.context(ui.state.draft, input.mouseX, input.mouseY);
    ui.state.contextCanTeleport = ui.state.draft.dimension == session.player.dimension;
    input.pressedMarker = 0;
    auto& history = session.history;
    history.cancelPoint();
    if (!ui.state.draft.y && history.available()) {
        history.requestPoint({view.displayedLayer(session.layer), ui.state.draft.x, ui.state.draft.z});
        ui.state.contextLoading = true;
        saveHistory();
    }
}

void Wayfinder::Impl::teleport() {
    auto client = session.client;
    auto player = client ? client->getLocalPlayer() : nullptr;
    if (!player || !player->isAlive()) throw std::runtime_error("Teleport unavailable while not in game.");
    if (!client->hasCommands() || player->getCommandPermissionLevel() < CommandPermissionLevel::GameDirectors)
        throw std::runtime_error("Teleport requires cheats and command permission.");
    auto point = ui.state.draft;
    int dimension = static_cast<int>(player->getDimensionId());
    // Validate before any world query, including a stale menu after dimension changes.
    teleportCommand(point, dimension);
    if (auto source = client->getRegion()) {
        auto chunk = source->getChunk(floorDiv(point.x, 16), floorDiv(point.z, 16));
        if (chunk && chunk->mLoadState->load(std::memory_order_acquire) == ChunkState::Loaded) {
            // Refresh the selected layer, never the roof above a cave/Nether map.
            auto sampleSettings = settings;
            sampleSettings.includeWater = sampleSettings.includeLeaves = true;
            auto cell = TerrainSampler::sample(*source, view.displayedLayer(session.layer), point.x, point.z, sampleSettings);
            if (!cell || !cell->floor() || cell->height + 1 >= source->getMaxHeight())
                throw std::runtime_error("No valid surface at this location.");
            point.y = cell->height;
        }
    }
    CommandContext context(teleportCommand(point, dimension),
        std::make_unique<PlayerCommandOrigin>(player->getLevel(), player->getOrCreateUniqueID()),
        static_cast<int>(CurrentCmdVersion::Latest));
    CommandRequestPacket request(CommandRequestPacketPayload(context, false));
    session.trail.stopRetrace();
    player->sendNetworkPacket(request);
    // Server command feedback stays visible in the HUD, including rejected requests.
    closeMap(true);
}

void Wayfinder::Impl::menuKey(int code, bool first) {
    try {
        if (code == VK_ESCAPE) {
            if (!first) return;
            if (ui.state.focus != UiAction::None) ui.state.cancelField();
            else ui.state.open(ui.state.page == MapMenu::Page::Entities ? MapMenu::Page::Settings : MapMenu::Page::Map);
            return;
        }
        if (ui.state.focus == UiAction::MapKey || ui.state.focus == UiAction::ToggleKey
            || ui.state.focus == UiAction::ZoomInKey || ui.state.focus == UiAction::ZoomOutKey) {
            if (!first) return;
            bool zoomKey = ui.state.focus == UiAction::ZoomInKey || ui.state.focus == UiAction::ZoomOutKey;
            if (!(zoomKey ? bindableZoomKey(code) : bindableMapKey(code))) {
                ui.state.message = zoomKey ? "Press a zoom key (+/- allowed); Esc cancels"
                                        : "Use a letter/number/F1-F12 (WASD/F reserved)";
                return;
            }
            auto changed = settings;
            if (ui.state.focus == UiAction::MapKey) changed.fullMapKey = code;
            else if (ui.state.focus == UiAction::ToggleKey) changed.toggleMinimapKey = code;
            else if (ui.state.focus == UiAction::ZoomInKey) changed.minimapZoomInKey = code;
            else changed.minimapZoomOutKey = code;
            saveSettings(mod.getConfigDir(), changed);
            settings = changed;
            ui.state.cancelField();
            ui.state.message = "Key saved";
            return;
        }
        if (ui.state.focus == UiAction::None) {
            if (first && code == VK_PRIOR) ui.state.scroll(-1);
            if (first && code == VK_NEXT) ui.state.scroll(1);
            return;
        }
        if (ui.state.textField()) { openText(); return; }
    } catch (std::exception const& ex) {
        ui.state.fail(ex.what());
        mod.getLogger().warn("Wayfinder UI: {}", ex.what());
    }
    ui.frame = ui.state.build(view.guiWidth, view.guiHeight, settings, session.navigation);
}

} // namespace wayfinder
