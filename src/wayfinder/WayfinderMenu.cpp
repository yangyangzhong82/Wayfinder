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
#include <string_view>

namespace wayfinder {
namespace {
std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr,
                                   0, nullptr, nullptr);
    if (!size) return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}
std::string pasteText() {
    if (!OpenClipboard(nullptr)) return {};
    struct Close {
        ~Close() { CloseClipboard(); }
    } close;
    auto data = GetClipboardData(CF_UNICODETEXT);
    if (!data) return {};
    auto text = static_cast<wchar_t const*>(GlobalLock(data));
    if (!text) return {};
    struct Unlock {
        HANDLE handle;
        ~Unlock() { GlobalUnlock(handle); }
    } unlock{data};
    auto capacity = std::min<std::size_t>(GlobalSize(data) / sizeof(wchar_t), 4096);
    std::size_t length{};
    while (length < capacity && text[length])
        ++length;
    return utf8({text, length});
}
std::string typedText(int code) {
    BYTE keyboard[256]{};
    if (!GetKeyboardState(keyboard)) return {};
    keyboard[VK_SHIFT] = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 0x80 : 0;
    wchar_t text[8]{};
    auto layout = GetKeyboardLayout(0);
    int length = ToUnicodeEx(code, MapVirtualKeyExW(code, MAPVK_VK_TO_VSC, layout), keyboard, text, 8, 4, layout);
    return length > 0 ? utf8({text, static_cast<std::size_t>(length)}) : std::string{};
}
} // namespace

void Wayfinder::Impl::Menu::refreshLocale(Settings const& settings) {
    // Refresh resource-pack/game language using the exposed mCode field.
    gameLanguage = *getI18n().getCurrentLanguage()->mCode;
    state.locale.select(settings.language, gameLanguage);
}

void Wayfinder::Impl::Menu::newWaypoint(Impl& app, bool atPointer) {
    Waypoint point;
    point.name = state.locale.tr("Waypoint");
    point.dimension = app.session.player.dimension;
    point.x = blockCoordinate(app.session.player.x);
    point.y = blockCoordinate(app.session.player.y);
    point.z = blockCoordinate(app.session.player.z);
    point.created = unixTime();
    if (atPointer && app.input.hasMouse && app.view.area.contains(app.input.mouseX, app.input.mouseY)) {
        auto world = app.rendering.renderedView.worldAt(
            (app.input.mouseX - app.view.area.x) / app.view.area.width * app.rendering.renderedView.width,
            (app.input.mouseY - app.view.area.y) / app.view.area.height * app.rendering.renderedView.height);
        point.x = blockCoordinate(world[0]);
        point.z = blockCoordinate(world[1]);
        auto cell = app.session.cache.get(app.session.layer, point.x, point.z);
        point.y = cell.floor() ? std::optional<int>(cell.height) : std::nullopt;
    }
    state.edit(std::move(point));
    app.input.panKeys.clear();
    app.input.dragging = false;
}

void Wayfinder::Impl::Menu::action(Impl& app, UiButton const& button) {
    app.input.panKeys.clear();
    app.input.dragging = false;
    refreshLocale(app.settings);
    if (button.action == UiAction::NewPlayer) {
        newWaypoint(app, false);
        return;
    }
    if (button.action == UiAction::Follow) {
        app.input.followPlayer(app);
        return;
    }
    if (button.action == UiAction::Locate) {
        state.open(MapMenu::Page::Locate);
        state.draft = {};
        state.draft.name = state.locale.tr("Waypoint");
        state.draft.dimension = app.session.player.dimension;
        state.draft.x = blockCoordinate(app.session.player.x);
        state.draft.z = blockCoordinate(app.session.player.z);
        state.draft.created = unixTime();
        return;
    }
    auto previousMenu = state;
    try {
        if (button.action == UiAction::Teleport) {
            if (state.page == MapMenu::Page::Context && button.enabled) teleport(app);
            return;
        }
        auto changedSettings = app.settings;
        auto changedNavigation = app.session.navigation;
        int change = state.action(button, changedSettings, changedNavigation);
        if (change == 1) {
            writeNavigation(app.session.navigationPath, app.session.identity, changedNavigation);
            app.session.navigation = std::move(changedNavigation);
            app.session.navigationDirty = false;
        } else if (change == 2) {
            saveSettings(app.mod.getConfigDir(), changedSettings);
            bool resample = app.settings.includeWater != changedSettings.includeWater
                || app.settings.terrainMode != changedSettings.terrainMode
                || app.settings.caveSwitchY != changedSettings.caveSwitchY
                || app.settings.includeLeaves != changedSettings.includeLeaves
                || app.settings.sampleRadiusChunks != changedSettings.sampleRadiusChunks;
            bool resize = app.settings.minimapPixels != changedSettings.minimapPixels
                || app.settings.fullscreenPixels != changedSettings.fullscreenPixels
                || app.settings.minimapBlocksPerPixel != changedSettings.minimapBlocksPerPixel;
            app.settings = changedSettings;
            app.session.lastEntities = {};
            if (!app.settings.showEntities) app.session.entities.clear();
            if (resample) app.session.sampler.reset();
            if (resize) app.rendering.pixels.clear();
            state.locale.select(app.settings.language, gameLanguage);
        } else if (change == 3 || change == 4) {
            auto point = state.draft;
            locateView(app.view.fullView, point.x, point.z);
            app.view.following = false;
            app.rendering.pixels.clear();
            if (change == 4) {
                auto cell = app.session.cache.get(app.session.layer, point.x, point.z);
                point.y = cell.floor() ? std::optional<int>(cell.height) : std::nullopt;
                state.edit(std::move(point));
            } else state.open(MapMenu::Page::Map);
        }
    } catch (std::exception const& ex) {
        state = std::move(previousMenu);
        state.fail(ex.what());
        app.mod.getLogger().warn("Wayfinder UI: {}", ex.what());
    }
    frame = state.build(app.view.guiWidth, app.view.guiHeight, app.settings, app.session.navigation);
}

void Wayfinder::Impl::Menu::openContext(Impl& app) {
    if (!app.input.hasMouse || !app.view.area.contains(app.input.mouseX, app.input.mouseY)) return;
    refreshLocale(app.settings);
    // Resolve once, before the pointer moves onto a menu action.
    newWaypoint(app, true);
    state.context(state.draft, app.input.mouseX, app.input.mouseY);
    app.input.pressedMarker = 0;
    auto& history = app.session.history;
    history.requestedPoint.reset();
    if (!state.draft.y && (history.archive || history.openTask.valid())) {
        history.requestedPoint = Session::History::PointRequest{app.session.layer, state.draft.x, state.draft.z};
        state.contextLoading = true;
        history.save(app, false);
    }
}

void Wayfinder::Impl::Menu::teleport(Impl& app) {
    auto client = app.session.client;
    auto player = client ? client->getLocalPlayer() : nullptr;
    if (!player || !player->isAlive()) throw std::runtime_error("Teleport unavailable while not in game.");
    if (!client->hasCommands() || player->getCommandPermissionLevel() < CommandPermissionLevel::GameDirectors)
        throw std::runtime_error("Teleport requires cheats and command permission.");
    auto point = state.draft;
    int dimension = static_cast<int>(player->getDimensionId());
    // Validate before any world query, including a stale menu after dimension changes.
    teleportCommand(point, dimension);
    if (auto source = client->getRegion()) {
        auto chunk = source->getChunk(floorDiv(point.x, 16), floorDiv(point.z, 16));
        if (chunk && chunk->mLoadState->load(std::memory_order_acquire) == ChunkState::Loaded) {
            // Refresh the selected layer, never the roof above a cave/Nether map.
            auto settings = app.settings;
            settings.includeWater = settings.includeLeaves = true;
            auto cell = TerrainSampler::sample(*source, app.session.layer, point.x, point.z, settings);
            if (!cell || !cell->floor() || cell->height + 1 >= source->getMaxHeight())
                throw std::runtime_error("No valid surface at this location.");
            point.y = cell->height;
        }
    }
    CommandContext context(teleportCommand(point, dimension),
        std::make_unique<PlayerCommandOrigin>(player->getLevel(), player->getOrCreateUniqueID()),
        static_cast<int>(CurrentCmdVersion::Latest));
    CommandRequestPacket request(CommandRequestPacketPayload(context, false));
    player->sendNetworkPacket(request);
    // Server command feedback stays visible in the HUD, including rejected requests.
    app.input.closeMap(app, true);
}

void Wayfinder::Impl::Menu::key(Impl& app, int code, bool first) {
    try {
        if (code == VK_ESCAPE) {
            if (!first) return;
            if (state.focus != UiAction::None) state.cancelField();
            else state.open(state.page == MapMenu::Page::Entities ? MapMenu::Page::Settings : MapMenu::Page::Map);
            return;
        }
        if (state.focus == UiAction::MapKey || state.focus == UiAction::ToggleKey
            || state.focus == UiAction::ZoomInKey || state.focus == UiAction::ZoomOutKey) {
            if (!first) return;
            bool zoomKey = state.focus == UiAction::ZoomInKey || state.focus == UiAction::ZoomOutKey;
            if (!(zoomKey ? bindableZoomKey(code) : bindableMapKey(code))) {
                state.message = zoomKey ? "Press a zoom key (+/- allowed); Esc cancels"
                                        : "Use a letter/number/F1-F12 (WASD/F reserved)";
                return;
            }
            auto changed = app.settings;
            if (state.focus == UiAction::MapKey) changed.fullMapKey = code;
            else if (state.focus == UiAction::ToggleKey) changed.toggleMinimapKey = code;
            else if (state.focus == UiAction::ZoomInKey) changed.minimapZoomInKey = code;
            else changed.minimapZoomOutKey = code;
            saveSettings(app.mod.getConfigDir(), changed);
            app.settings = changed;
            state.cancelField();
            state.message = "Key saved";
            return;
        }
        if (state.focus == UiAction::None) {
            if (first && code == VK_PRIOR) state.scroll(-1);
            if (first && code == VK_NEXT) state.scroll(1);
            return;
        }
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        if (code == VK_RETURN || code == VK_TAB) {
            state.commitField();
            return;
        }
        if (code == VK_BACK) {
            state.backspace();
            return;
        }
        if (ctrl && code == 'A') {
            state.selectAll = true;
            return;
        }
        if (ctrl && code == 'V' && first) {
            state.append(pasteText());
            return;
        }
        if (!ctrl && !(GetAsyncKeyState(VK_MENU) & 0x8000)) state.append(typedText(code));
    } catch (std::exception const& ex) {
        state.fail(ex.what());
        app.mod.getLogger().warn("Wayfinder UI: {}", ex.what());
    }
    frame = state.build(app.view.guiWidth, app.view.guiHeight, app.settings, app.session.navigation);
}

} // namespace wayfinder
