#include "wayfinder/WayfinderInternal.h"
#include "wayfinder/MapMarkers.h"
#include "ll/api/mod/NativeMod.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/input/ClientInputHandler.h"
#include <utility>

namespace wayfinder {
bool Wayfinder::Impl::foreground() {
    DWORD process{};
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}
bool Wayfinder::Impl::hud(IClientInstance& client) {
    return client.isPlaying() && client.getTopScreenName() == "hud_screen";
}
namespace {
struct InputTransition {
    bool& active;
    explicit InputTransition(bool& value) : active(value) { active = true; }
    ~InputTransition() { active = false; }
};
} // namespace

void Wayfinder::Impl::Input::followPlayer(Impl& app) {
    pressedMarker = 0;
    app.view.follow(app.session.player.x, app.session.player.z);
    app.session.history.requestedView.reset();
    app.session.history.requestedPoint.reset();
    panKeys.clear();
    dragging = false;
    app.rendering.pixels.clear();
    app.ui.state.open(MapMenu::Page::Map);
}

void Wayfinder::Impl::Input::closeMap(Impl& app, bool grab) {
    app.ui.textInput.cancel();
    app.ui.nativeField = UiAction::None;
    pressedMarker = 0;
    if (!app.view.fullscreen) return;
    app.view.fullscreen = false;
    app.session.history.requestedPoint.reset();
    if (mapMouse) mapMouse->release();
    mapWindow = nullptr;
    hasMouse = false;
    mouseX = mouseY = -1;
    app.ui.state.open(MapMenu::Page::Map);
    app.ui.frame = {};
    dragging = false;
    panKeys.clear();
    InputTransition transition(transitioning);
    if (app.session.client) {
        if (auto input = app.session.client->getInput()) {
            input->releaseButtonsAndSticks();
            input->resetPlayerState();
        }
        app.session.client->setSuspendInput(false);
        if (grab && restoreMouse && foreground() && hud(*app.session.client)) app.session.client->grabMouse();
    }
    app.rendering.pixels.clear();
}

void Wayfinder::Impl::Input::openMap(Impl& app) {
    if (!app.session.client || !hud(*app.session.client) || !app.session.client->isInGameInputEnabled() ||
        !foreground())
        return;
    restoreMouse = app.session.client->getMouseGrabbed();
    InputTransition transition(transitioning);
    // Mark ownership before calling engine APIs so exception cleanup restores input.
    app.view.fullscreen = true;
    mapWindow = GetForegroundWindow();
    mapMouse->acquire(*app.session.client);
    if (auto input = app.session.client->getInput()) {
        input->releaseButtonsAndSticks();
        input->resetPlayerState();
    }
    app.session.client->resetPlayerMovement();
    app.session.client->setSuspendInput(true);
    app.session.client->releaseMouse();
    // The map uses the real desktop pointer, so it may leave the client area.
    ClipCursor(nullptr);
    app.view.following = true;
    dragging = false;
    hasMouse = false;
    mouseX = mouseY = -1;
    app.view.follow(app.session.player.x, app.session.player.z);
    panKeys.clear();
    lastPan = Clock::now();
    app.rendering.pixels.clear();
}

void Wayfinder::Impl::Input::key(Impl& app, ll::event::KeyInputEvent& event) {
    if (transitioning) return;
    int code = event.keyCode();
    if (!event.isDown()) {
        heldKeys.erase(code);
        panKeys.erase(code);
        if (consumedKeys.erase(code)) event.cancel();
        return;
    }
    if (event.isCancelled()) return;
    bool first = heldKeys.insert(code).second;
    if (app.ui.textInput.active()) {
        consumedKeys.insert(code);
        event.cancel();
        return;
    }
    if (!app.session.client || !foreground() || !hud(*app.session.client)) return;
    if (app.view.fullscreen && app.ui.state.page != MapMenu::Page::Map) {
        consumedKeys.insert(code);
        event.cancel();
        app.ui.key(app, code, first);
        return;
    }
    if (code == app.settings.fullMapKey) {
        if (first) {
            if (app.view.fullscreen) closeMap(app, true);
            else openMap(app);
        }
        consumedKeys.insert(code);
        event.cancel();
        return;
    }
    if (code == app.settings.toggleMinimapKey) {
        if (first) {
            auto changed = app.settings;
            changed.showMinimap = !changed.showMinimap;
            saveSettings(app.mod.getConfigDir(), changed);
            app.settings = changed;
        }
        consumedKeys.insert(code);
        event.cancel();
        return;
    }
    if (!app.view.fullscreen && app.settings.showMinimap
        && (code == app.settings.minimapZoomInKey || code == app.settings.minimapZoomOutKey)) {
        consumedKeys.insert(code);
        event.cancel();
        if (first) app.ui.action(app, UiButton{{}, {}, code == app.settings.minimapZoomInKey
                                             ? UiAction::MinimapZoomIn : UiAction::MinimapZoomOut});
        return;
    }
    if (!app.view.fullscreen) return;
    consumedKeys.insert(code);
    event.cancel();
    if (!first) return;
    switch (code) {
    case VK_ESCAPE:
        closeMap(app, true);
        break;
    case VK_HOME:
        followPlayer(app);
        break;
    case 'F': {
        auto const& bounds = app.session.history.bounds;
        auto layer = app.view.displayedLayer(app.session.layer);
        auto found = bounds.find(layer);
        app.view.fit(app.session.cache.bounds(layer),
                     found == bounds.end() ? std::nullopt : std::optional<MapBounds>(found->second));
    } break;
    case VK_ADD:
    case VK_OEM_PLUS:
        app.view.zoom(0.8, hasMouse, mouseX, mouseY);
        break;
    case VK_SUBTRACT:
    case VK_OEM_MINUS:
        app.view.zoom(1.25, hasMouse, mouseX, mouseY);
        break;
    case VK_LEFT:
    case 'A':
    case VK_RIGHT:
    case 'D':
    case VK_UP:
    case 'W':
    case VK_DOWN:
    case 'S':
        panKeys.insert(code);
        app.view.following = false;
        break;
    default:
        break;
    }
}

void Wayfinder::Impl::Input::mouse(Impl& app, ll::event::MouseInputEvent& event) {
    if (transitioning) return;
    int button = static_cast<unsigned char>(event.actionButtonId());
    int data = event.buttonData();
    if (!app.view.fullscreen) {
        if (data == 0 && consumedMouse.erase(button)) event.cancel();
        return;
    }
    if (!app.session.client || !hud(*app.session.client) || !foreground()) {
        closeMap(app, false);
        return;
    }
    // Consume buttons even if their release arrives outside the client area.
    if (button >= 1 && button <= 3) {
        if (data != 0) consumedMouse.insert(button);
        else consumedMouse.erase(button);
    }
    event.cancel();
    if (app.ui.textInput.active()) return;
    // One absolute coordinate source only. Relative/raw event coordinates
    // must never be mixed with the Windows desktop pointer.
    if (!pollPointer(app)) return;
    float x = mouseX, y = mouseY;
    // Build hit targets from the current menu, never from a stale render.
    app.ui.frame = app.ui.state.build(app.view.guiWidth, app.view.guiHeight, app.settings, app.session.navigation);
    bool wasModal = app.ui.frame.modal;
    auto hit = app.ui.frame.hit(x, y);
    if (button == 1) {
        if (data != 0) {
            pressedMarker = 0;
            if (hit) {
                auto action = *hit;
                app.ui.action(app, action);
            } else if (app.ui.state.page == MapMenu::Page::Context) {
                if (!app.ui.frame.panel.contains(x, y)) app.ui.state.open(MapMenu::Page::Map);
                dragging = false;
            } else {
                dragging = !wasModal && app.view.area.contains(x, y);
                if (dragging) {
                    auto markers = layoutMarkers(app.session.navigation, app.rendering.renderedView, app.view.area,
                        app.view.displayedLayer(app.session.layer).dimension, app.settings.showWaypoints, app.settings.showNavigation, false);
                    pressedMarker = hitMarker(markers, x, y);
                    pressX = x;
                    pressY = y;
                }
            }
        } else {
            auto selected = std::exchange(pressedMarker, 0);
            dragging = false;
            if (selected && !wasModal && app.view.area.contains(x, y) && std::hypot(x - pressX, y - pressY) <= 3) {
                auto action = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? UiAction::NavigateExisting : UiAction::OpenWaypoint;
                app.ui.action(app, UiButton{{}, {}, action, selected});
            }
        }
    }
    if (button == 2 && data != 0 && (!wasModal || app.ui.state.page == MapMenu::Page::Context)
        && app.view.area.contains(x, y)) app.ui.openContext(app);
    if (button == 4 && data != 0) {
        if (wasModal) {
            try {
                app.ui.state.scroll(data > 0 ? -1 : 1);
            } catch (std::exception const& ex) {
                app.ui.state.fail(ex.what());
                app.mod.getLogger().warn("Wayfinder UI: {}", ex.what());
            }
        } else if (app.view.area.contains(x, y)) app.view.zoom(data > 0 ? 0.8 : 1.25, hasMouse, mouseX, mouseY);
    }
}

void Wayfinder::Impl::Input::movePointer(Impl& app, float x, float y) {
    if (app.ui.state.page == MapMenu::Page::Map && dragging && hasMouse && app.view.area.width > 0 &&
        app.view.area.height > 0) {
        if (pressedMarker) {
            if (std::hypot(x - pressX, y - pressY) > 3) {
                pressedMarker = 0;
                app.view.drag(x - pressX, y - pressY);
            }
        } else app.view.drag(x - mouseX, y - mouseY);
    }
    mouseX = x;
    mouseY = y;
    hasMouse = true;
}

bool Wayfinder::Impl::Input::pollPointer(Impl& app) {
    auto unavailable = [&] {
        pressedMarker = 0;
        dragging = false;
        hasMouse = false;
        mouseX = mouseY = -1;
        return false;
    };
    if (app.view.guiWidth <= 0 || app.view.guiHeight <= 0 || !mapWindow || !IsWindow(mapWindow) ||
        GetForegroundWindow() != mapWindow)
        return unavailable();
    POINT point{};
    RECT bounds{};
    if (!GetCursorPos(&point) || !ScreenToClient(mapWindow, &point) || !GetClientRect(mapWindow, &bounds) ||
        bounds.right <= bounds.left || bounds.bottom <= bounds.top)
        return unavailable();
    if (!PtInRect(&bounds, point)) return unavailable();
    // A release may occur outside the window and never reach MouseInputEvent.
    // Do not resume the old drag on re-entry or treat re-entry as movement.
    if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) dragging = false;
    movePointer(app, static_cast<float>(point.x - bounds.left) * app.view.guiWidth / (bounds.right - bounds.left),
                static_cast<float>(point.y - bounds.top) * app.view.guiHeight / (bounds.bottom - bounds.top));
    return true;
}

void Wayfinder::Impl::Input::panHeldKeys(Impl& app) {
    auto now = Clock::now();
    double seconds = std::chrono::duration<double>(now - lastPan).count();
    lastPan = now;
    auto down = [&](int a, int b) { return panKeys.contains(a) || panKeys.contains(b); };
    double dx = int(down(VK_RIGHT, 'D')) - int(down(VK_LEFT, 'A'));
    double dz = int(down(VK_DOWN, 'S')) - int(down(VK_UP, 'W'));
    if (dx || dz) {
        app.view.fullView.pan(dx, dz, seconds);
        app.view.following = false;
    }
}

} // namespace wayfinder
