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

void Wayfinder::Impl::closeMap(bool grab) {
    ui.textInput.cancel();
    ui.nativeField = UiAction::None;
    input.pressedMarker = 0;
    if (!view.fullscreen) return;
    view.fullscreen = false;
    invalidateDisplayedMap();
    if (input.mapMouse) input.mapMouse->release();
    input.mapWindow = nullptr;
    input.hasMouse = false;
    input.mouseX = input.mouseY = -1;
    ui.state.open(MapMenu::Page::Map);
    ui.frame = {};
    InputTransition transition(input.transitioning);
    if (session.client) {
        if (auto input = session.client->getInput()) {
            input->releaseButtonsAndSticks();
            input->resetPlayerState();
        }
        session.client->setSuspendInput(false);
        if (grab && input.restoreMouse && foreground() && hud(*session.client)) session.client->grabMouse();
    }
}

void Wayfinder::Impl::openMap() {
    if (!session.client || !hud(*session.client) || !session.client->isInGameInputEnabled() ||
        !foreground())
        return;
    input.restoreMouse = session.client->getMouseGrabbed();
    renderRecovery.retryNow(Clock::now());
    InputTransition transition(input.transitioning);
    // Mark ownership before calling engine APIs so exception cleanup restores input.
    view.fullscreen = true;
    input.mapWindow = GetForegroundWindow();
    input.mapMouse->acquire(*session.client);
    if (auto input = session.client->getInput()) {
        input->releaseButtonsAndSticks();
        input->resetPlayerState();
    }
    session.client->resetPlayerMovement();
    session.client->setSuspendInput(true);
    session.client->releaseMouse();
    // The map uses the real desktop pointer, so it may leave the client area.
    ClipCursor(nullptr);
    input.hasMouse = false;
    input.mouseX = input.mouseY = -1;
    followPlayer();
    input.lastPan = Clock::now();
}

void Wayfinder::Impl::key(ll::event::KeyInputEvent& event) {
    if (input.transitioning) return;
    int code = event.keyCode();
    if (!event.isDown()) {
        input.heldKeys.erase(code);
        input.panKeys.erase(code);
        if (input.consumedKeys.erase(code)) event.cancel();
        return;
    }
    if (event.isCancelled()) return;
    bool first = input.heldKeys.insert(code).second;
    if (ui.textInput.active()) {
        input.consumedKeys.insert(code);
        event.cancel();
        return;
    }
    if (!session.client || !foreground() || !hud(*session.client)) return;
    if (view.fullscreen && ui.state.page != MapMenu::Page::Map) {
        input.consumedKeys.insert(code);
        event.cancel();
        menuKey(code, first);
        return;
    }
    if (code == settings.fullMapKey) {
        if (first) {
            if (view.fullscreen) closeMap(true);
            else openMap();
        }
        input.consumedKeys.insert(code);
        event.cancel();
        return;
    }
    if (code == settings.toggleMinimapKey) {
        if (first) {
            auto changed = settings;
            changed.showMinimap = !changed.showMinimap;
            saveSettings(mod.getConfigDir(), changed);
            settings = changed;
            if (changed.showMinimap) renderRecovery.retryNow(Clock::now());
        }
        input.consumedKeys.insert(code);
        event.cancel();
        return;
    }
    if (!view.fullscreen && settings.showMinimap
        && (code == settings.minimapZoomInKey || code == settings.minimapZoomOutKey)) {
        input.consumedKeys.insert(code);
        event.cancel();
        if (first) menuAction(UiButton{{}, {}, code == settings.minimapZoomInKey
                                             ? UiAction::MinimapZoomIn : UiAction::MinimapZoomOut});
        return;
    }
    if (!view.fullscreen) return;
    input.consumedKeys.insert(code);
    event.cancel();
    if (!first) return;
    switch (code) {
    case VK_ESCAPE:
        closeMap(true);
        break;
    case VK_HOME:
        followPlayer();
        break;
    case 'F':
        fitKnownArea();
        break;
    case VK_ADD:
    case VK_OEM_PLUS:
        view.zoom(0.8, input.hasMouse, input.mouseX, input.mouseY);
        break;
    case VK_SUBTRACT:
    case VK_OEM_MINUS:
        view.zoom(1.25, input.hasMouse, input.mouseX, input.mouseY);
        break;
    case VK_LEFT:
    case 'A':
    case VK_RIGHT:
    case 'D':
    case VK_UP:
    case 'W':
    case VK_DOWN:
    case 'S':
        input.panKeys.insert(code);
        view.following = false;
        break;
    default:
        break;
    }
}

void Wayfinder::Impl::mouse(ll::event::MouseInputEvent& event) {
    if (input.transitioning) return;
    int button = static_cast<unsigned char>(event.actionButtonId());
    int data = event.buttonData();
    if (!view.fullscreen) {
        if (data == 0 && input.consumedMouse.erase(button)) event.cancel();
        return;
    }
    // Mouse messages generated while the embedded text editor is focused can
    // still reach the game input hook. They belong to the editor and must not
    // trigger the map's foreground-loss shutdown path.
    if (ui.textInput.active()) {
        event.cancel();
        return;
    }
    if (!session.client || !hud(*session.client) || !foreground()) {
        closeMap(false);
        return;
    }
    // Consume buttons even if their release arrives outside the client area.
    if (button >= 1 && button <= 3) {
        if (data != 0) input.consumedMouse.insert(button);
        else input.consumedMouse.erase(button);
    }
    event.cancel();
    if (ui.textInput.active()) return;
    // One absolute coordinate source only. Relative/raw event coordinates
    // must never be mixed with the Windows desktop pointer.
    if (!input.pollPointer(view, ui.state.page == MapMenu::Page::Map)) return;
    float x = input.mouseX, y = input.mouseY;
    // Build hit targets from the current menu, never from a stale render.
    ui.frame = ui.state.build(view.guiWidth, view.guiHeight, settings, session.navigation);
    bool wasModal = ui.frame.modal;
    auto hit = ui.frame.hit(x, y);
    if (button == 1) {
        if (data != 0) {
            input.pressedMarker = 0;
            if (hit) {
                auto action = *hit;
                menuAction(action);
            } else if (ui.state.page == MapMenu::Page::Context) {
                if (!ui.frame.panel.contains(x, y)) ui.state.open(MapMenu::Page::Map);
                input.dragging = false;
            } else {
                input.dragging = !wasModal && view.area.contains(x, y);
                if (input.dragging) {
                    auto markers = layoutMarkers(session.navigation, rendering.displayedView(), view.area,
                        view.displayedLayer(session.layer).dimension, settings.showWaypoints, settings.showNavigation, false);
                    input.pressedMarker = hitMarker(markers, x, y);
                    input.pressX = x;
                    input.pressY = y;
                }
            }
        } else {
            auto selected = std::exchange(input.pressedMarker, 0);
            input.dragging = false;
            if (selected && !wasModal && view.area.contains(x, y) && std::hypot(x - input.pressX, y - input.pressY) <= 3) {
                auto action = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? UiAction::NavigateExisting : UiAction::OpenWaypoint;
                menuAction(UiButton{{}, {}, action, selected});
            }
        }
    }
    if (button == 2 && data != 0 && (!wasModal || ui.state.page == MapMenu::Page::Context)
        && view.area.contains(x, y)) openContext();
    if (button == 4 && data != 0) {
        if (wasModal) {
            try {
                ui.state.scroll(data > 0 ? -1 : 1);
            } catch (std::exception const& ex) {
                ui.state.fail(ex.what());
                mod.getLogger().warn("Wayfinder UI: {}", ex.what());
            }
        } else if (view.area.contains(x, y)) view.zoom(data > 0 ? 0.8 : 1.25, input.hasMouse, input.mouseX, input.mouseY);
    }
}

void Wayfinder::Impl::Input::movePointer(WayfinderView& view, bool mapPage, float x, float y) {
    if (mapPage && dragging && hasMouse && view.area.width > 0 &&
        view.area.height > 0) {
        if (pressedMarker) {
            if (std::hypot(x - pressX, y - pressY) > 3) {
                pressedMarker = 0;
                view.drag(x - pressX, y - pressY);
            }
        } else view.drag(x - mouseX, y - mouseY);
    }
    mouseX = x;
    mouseY = y;
    hasMouse = true;
}

bool Wayfinder::Impl::Input::pollPointer(WayfinderView& view, bool mapPage) {
    auto unavailable = [&] {
        pressedMarker = 0;
        dragging = false;
        hasMouse = false;
        mouseX = mouseY = -1;
        return false;
    };
    if (view.guiWidth <= 0 || view.guiHeight <= 0 || !mapWindow || !IsWindow(mapWindow) ||
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
    movePointer(view, mapPage, static_cast<float>(point.x - bounds.left) * view.guiWidth / (bounds.right - bounds.left),
                static_cast<float>(point.y - bounds.top) * view.guiHeight / (bounds.bottom - bounds.top));
    return true;
}

void Wayfinder::Impl::Input::panHeldKeys(WayfinderView& view) {
    auto now = Clock::now();
    double seconds = std::chrono::duration<double>(now - lastPan).count();
    lastPan = now;
    auto down = [&](int a, int b) { return panKeys.contains(a) || panKeys.contains(b); };
    double dx = int(down(VK_RIGHT, 'D')) - int(down(VK_LEFT, 'A'));
    double dz = int(down(VK_DOWN, 'S')) - int(down(VK_UP, 'W'));
    if (dx || dz) {
        view.fullView.pan(dx, dz, seconds);
        view.following = false;
    }
}

} // namespace wayfinder
