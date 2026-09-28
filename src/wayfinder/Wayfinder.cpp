#include "wayfinder/Wayfinder.h"
#include "wayfinder/MapRenderer.h"
#include "wayfinder/MapStorage.h"
#include "wayfinder/Settings.h"
#include "wayfinder/TerrainSampler.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/service/Bedrock.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/GuiData.h"
#include "mc/client/input/ClientInputHandler.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/player/LocalPlayer.h"

#include <Windows.h>
#include <chrono>
#include <fstream>
#include <future>
#include <mutex>
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace wayfinder {
namespace {
using Clock = std::chrono::steady_clock;
bool foreground() {
    DWORD process{};
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}
bool hud(IClientInstance& client) { return client.isPlaying() && client.getTopScreenName() == "hud_screen"; }
struct InputTransition {
    bool& active;
    explicit InputTransition(bool& value) : active(value) { active = true; }
    ~InputTransition() { active = false; }
};
Settings loadSettings(std::filesystem::path const& directory) {
    Settings s;
    auto     path = directory / "config.json";
    if (std::filesystem::exists(path)) {
        std::ifstream input(path);
        auto          j = nlohmann::json::parse(input);
#define WF_READ(name) s.name = j.value(#name, s.name)
        WF_READ(fullMapKey);
        WF_READ(toggleMinimapKey);
        WF_READ(sampleRadiusChunks);
        WF_READ(columnsPerTick);
        WF_READ(samplingBudgetMicros);
        WF_READ(maxCachedChunks);
        WF_READ(minimapPixels);
        WF_READ(fullscreenPixels);
        WF_READ(refreshMilliseconds);
        WF_READ(autosaveSeconds);
        WF_READ(minimapSize);
        WF_READ(minimapBlocksPerPixel);
        WF_READ(showMinimap);
        WF_READ(includeWater);
        WF_READ(includeLeaves);
        WF_READ(cacheProfile);
#undef WF_READ
    } else {
        std::filesystem::create_directories(directory);
        nlohmann::json j;
#define WF_WRITE(name) j[#name] = s.name
        WF_WRITE(fullMapKey);
        WF_WRITE(toggleMinimapKey);
        WF_WRITE(sampleRadiusChunks);
        WF_WRITE(columnsPerTick);
        WF_WRITE(samplingBudgetMicros);
        WF_WRITE(maxCachedChunks);
        WF_WRITE(minimapPixels);
        WF_WRITE(fullscreenPixels);
        WF_WRITE(refreshMilliseconds);
        WF_WRITE(autosaveSeconds);
        WF_WRITE(minimapSize);
        WF_WRITE(minimapBlocksPerPixel);
        WF_WRITE(showMinimap);
        WF_WRITE(includeWater);
        WF_WRITE(includeLeaves);
        WF_WRITE(cacheProfile);
#undef WF_WRITE
        std::ofstream output(path);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << j.dump(4);
    }
    if (s.fullMapKey < 1 || s.fullMapKey > 255 || s.toggleMinimapKey < 1 || s.toggleMinimapKey > 255
        || s.fullMapKey == s.toggleMinimapKey || s.fullMapKey == VK_ESCAPE || s.toggleMinimapKey == VK_ESCAPE)
        throw std::runtime_error("Map keys must be distinct virtual-key codes 1..255 (except Esc)");
    s.sampleRadiusChunks    = std::clamp(s.sampleRadiusChunks, 1, 16);
    s.columnsPerTick        = std::clamp(s.columnsPerTick, 64, 8192);
    s.samplingBudgetMicros  = std::clamp(s.samplingBudgetMicros, 100, 5000);
    s.maxCachedChunks       = std::clamp(s.maxCachedChunks, 256, 65536);
    s.minimapPixels         = std::clamp(s.minimapPixels, 64, 256);
    s.fullscreenPixels      = std::clamp(s.fullscreenPixels, 128, 768);
    s.refreshMilliseconds   = std::clamp(s.refreshMilliseconds, 50, 1000);
    s.autosaveSeconds       = std::clamp(s.autosaveSeconds, 10, 600);
    s.minimapSize           = std::clamp(s.minimapSize, 64.0f, 256.0f);
    s.minimapBlocksPerPixel = std::clamp(s.minimapBlocksPerPixel, 0.5, 16.0);
    if (s.cacheProfile.size() > 1024) throw std::runtime_error("cacheProfile is too long");
    return s;
}
} // namespace

struct Wayfinder::Impl {
    ll::mod::NativeMod&                 mod;
    Settings                            settings;
    MapCache                            cache;
    TerrainSampler                      sampler;
    std::recursive_mutex                mutex;
    std::vector<ll::event::ListenerPtr> listeners;
    std::unique_ptr<MapRenderer>        renderer;
    IClientInstance*                    client{}; // Valid only until ClientExitLevelEvent. Never captured by a worker.
    bool    enabled{}, fullscreen{}, following{true}, restoreMouse{}, transitioning{}, resetTexture{};
    bool    dragging{}, hasMouse{}, renderFailed{};
    int     dimension{};
    double  playerX{}, playerY{}, playerZ{};
    float   yaw{}, mouseX{}, mouseY{};
    float   mouseScaleX{1}, mouseScaleY{1};
    float   guiWidth{}, guiHeight{};
    MapRect area;
    MapView fullView, renderedView;
    std::vector<std::uint32_t> pixels;
    std::unordered_set<int>    heldKeys, consumedKeys;
    std::unordered_set<int>    consumedMouse;
    std::uint64_t              imageRevision{}, rasterRevision{}, savedRevision{};
    int                        rasterDimension = std::numeric_limits<int>::min();
    std::string                identity;
    std::filesystem::path      savePath;
    std::future<void>          saveTask;
    Clock::time_point          lastRaster{}, lastSave{}, lastError{};

    explicit Impl(ll::mod::NativeMod& owner)
    : mod(owner),
      settings(loadSettings(owner.getConfigDir())),
      cache(settings.maxCachedChunks) {}

    template <class Function>
    void guarded(Function&& function) {
        std::lock_guard lock(mutex);
        if (!enabled) return;
        try {
            function();
        } catch (std::exception const& ex) {
            if (Clock::now() - lastError > std::chrono::seconds(5)) {
                mod.getLogger().error("Wayfinder: {}", ex.what());
                lastError = Clock::now();
            }
            closeMap(false);
        }
    }
    template <class Event>
    void subscribe(std::weak_ptr<Wayfinder> weak, void (Impl::*handler)(Event&)) {
        auto listener = ll::event::EventBus::getInstance().emplaceListener<Event>([weak, handler](Event& event) {
            if (auto self = weak.lock()) self->mImpl->guarded([&] { (self->mImpl.get()->*handler)(event); });
        });
        if (!listener) throw std::runtime_error("Unable to register Wayfinder event listener");
        listeners.push_back(std::move(listener));
    }
    void finishSave() {
        if (!saveTask.valid()) return;
        try {
            saveTask.get();
        } catch (std::exception const& ex) {
            mod.getLogger().error("Wayfinder map save failed: {}", ex.what());
            savedRevision = std::numeric_limits<std::uint64_t>::max();
        }
    }
    void save(bool force) {
        if (identity.empty() || cache.size() == 0) return;
        if (saveTask.valid()) {
            if (!force && saveTask.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            finishSave();
        }
        if (cache.revision() == savedRevision) return;
        auto records  = cache.snapshot();
        auto path     = savePath;
        auto key      = identity;
        auto revision = cache.revision();
        saveTask =
            std::async(std::launch::async, [path, key, records = std::move(records)] { writeMap(path, key, records); });
        savedRevision = revision;
        lastSave      = Clock::now();
    }
    void beginSession(IClientInstance& ci) {
        finishSave();
        client = &ci;
        cache.clear();
        sampler.reset();
        pixels.clear();
        resetTexture = true;
        renderFailed = false;
        dimension    = static_cast<int>(ci.getLocalPlayer()->getDimensionId());
        auto levelId = ci.getLevel()->getLevelId();
        if (!settings.cacheProfile.empty()) identity = "profile:" + settings.cacheProfile;
        else if (!ci.isPrimaryLevelMultiplayer() && !levelId.empty()) identity = "local:" + levelId;
        else identity = "session:" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        savePath = mod.getDataDir() / "maps" / storageName(identity);
        try {
            cache.restore(readMap(savePath, identity, cache.capacity()));
        } catch (std::exception const& ex) {
            // Preserve unreadable data: write this run to a distinct recovery file.
            mod.getLogger().warn("Wayfinder could not load map: {}", ex.what());
            savePath += ".recovery-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        }
        savedRevision = cache.revision();
        lastSave      = Clock::now();
        mod.getLogger().info("Wayfinder map: {} ({} cached chunks)", savePath.string(), cache.size());
        if (identity.starts_with("session:"))
            mod.getLogger().info(
                "Multiplayer history is isolated per session; set a unique cacheProfile for this world to reuse it."
            );
    }
    void closeMap(bool grab) {
        if (!fullscreen) return;
        fullscreen = false;
        dragging   = false;
        InputTransition transition(transitioning);
        if (client) {
            if (auto input = client->getInput()) {
                input->releaseButtonsAndSticks();
                input->resetPlayerState();
            }
            client->setSuspendInput(false);
            if (grab && restoreMouse && foreground() && hud(*client)) client->grabMouse();
        }
        pixels.clear();
    }
    void openMap() {
        if (!client || !hud(*client) || !client->isInGameInputEnabled() || !foreground()) return;
        restoreMouse = client->getMouseGrabbed();
        InputTransition transition(transitioning);
        // Mark ownership before calling engine APIs so exception cleanup restores input.
        fullscreen = true;
        if (auto input = client->getInput()) {
            input->releaseButtonsAndSticks();
            input->resetPlayerState();
        }
        client->resetPlayerMovement();
        client->setSuspendInput(true);
        client->releaseMouse();
        following               = true;
        dragging                = false;
        hasMouse                = false;
        fullView.centerX        = playerX;
        fullView.centerZ        = playerZ;
        fullView.blocksPerPixel = 2.0;
        pixels.clear();
    }
    void endSession() {
        closeMap(false);
        save(true);
        client = nullptr;
        identity.clear();
        cache.clear();
        sampler.reset();
        pixels.clear();
        heldKeys.clear();
        consumedKeys.clear();
        consumedMouse.clear();
        dragging     = false;
        resetTexture = true;
    }
    void tick(ll::event::ClientLevelTickEvent& event) {
        if (!client) {
            auto ci = ll::service::getClientInstance();
            if (!ci || !ci->isPrimaryClient() || !ci->getLocalPlayer() || ci->getLevel() != &event.level()) return;
            beginSession(*ci);
        }
        if (client->getLevel() != &event.level()) return;
        auto player = client->getLocalPlayer();
        auto source = client->getRegion();
        if (!player || !source) return;
        if (!foreground()) {
            closeMap(false);
            heldKeys.clear();
            consumedKeys.clear();
            consumedMouse.clear();
            return;
        }
        if (fullscreen && !hud(*client)) closeMap(false);
        auto const& pos = player->getPosition();
        if (!std::isfinite(pos.x) || !std::isfinite(pos.z) || std::abs(pos.x) > 29999000 || std::abs(pos.z) > 29999000)
            return;
        playerX          = pos.x;
        playerY          = pos.y;
        playerZ          = pos.z;
        yaw              = player->getRotation().y;
        int newDimension = static_cast<int>(player->getDimensionId());
        if (dimension != newDimension) {
            closeMap(true);
            dimension = newDimension;
            sampler.reset();
            pixels.clear();
        }
        // Surface mode is intentionally limited to Overworld in this first implementation.
        if (dimension == 0)
            sampler.tick(
                *source,
                dimension,
                static_cast<int>(std::floor(playerX)),
                static_cast<int>(std::floor(playerZ)),
                cache,
                settings
            );
        if (saveTask.valid() && saveTask.wait_for(std::chrono::seconds(0)) == std::future_status::ready) finishSave();
        if (Clock::now() - lastSave >= std::chrono::seconds(settings.autosaveSeconds)) save(false);
    }
    void changed(ll::event::BlockChangedEvent& event) {
        if (!client || client->getRegion() != &event.blockSource()) return;
        sampler.markDirty(static_cast<int>(event.blockSource().getDimensionId()), event.pos().x, event.pos().z);
    }
    void exit(ll::event::ClientExitLevelEvent& event) {
        if (client == &event.self()) endSession();
    }
    void fit() {
        if (auto bounds = cache.bounds(dimension)) {
            fullView.centerX        = (bounds->minX + bounds->maxX) * 0.5;
            fullView.centerZ        = (bounds->minZ + bounds->maxZ) * 0.5;
            fullView.blocksPerPixel = std::clamp(
                1.1
                    * std::max(
                        (bounds->maxX - bounds->minX) / fullView.width,
                        (bounds->maxZ - bounds->minZ) / fullView.height
                    ),
                0.5,
                128.0
            );
            following = false;
        }
    }
    void zoom(double factor) {
        double x = fullView.width * 0.5, y = fullView.height * 0.5;
        if (hasMouse && area.contains(mouseX, mouseY)) {
            x         = (mouseX - area.x) / area.width * fullView.width;
            y         = (mouseY - area.y) / area.height * fullView.height;
            following = false;
        }
        fullView.zoomAt(factor, x, y);
    }
    void key(ll::event::KeyInputEvent& event) {
        if (transitioning) return;
        int code = event.keyCode();
        if (!event.isDown()) {
            heldKeys.erase(code);
            if (consumedKeys.erase(code)) event.cancel();
            return;
        }
        if (event.isCancelled()) return;
        bool first = heldKeys.insert(code).second;
        if (!client || !foreground() || !hud(*client)) return;
        if (code == settings.fullMapKey) {
            if (first) {
                if (fullscreen) closeMap(true);
                else openMap();
            }
            consumedKeys.insert(code);
            event.cancel();
            return;
        }
        if (code == settings.toggleMinimapKey) {
            if (first) settings.showMinimap = !settings.showMinimap;
            consumedKeys.insert(code);
            event.cancel();
            return;
        }
        if (!fullscreen) return;
        consumedKeys.insert(code);
        event.cancel();
        if (!first) return;
        double step = fullView.blocksPerPixel * 24;
        switch (code) {
        case VK_ESCAPE:
            closeMap(true);
            break;
        case VK_HOME:
            following = true;
            break;
        case 'F':
            fit();
            break;
        case VK_ADD:
        case VK_OEM_PLUS:
            zoom(0.8);
            break;
        case VK_SUBTRACT:
        case VK_OEM_MINUS:
            zoom(1.25);
            break;
        case VK_LEFT:
        case 'A':
            fullView.centerX -= step;
            following         = false;
            break;
        case VK_RIGHT:
        case 'D':
            fullView.centerX += step;
            following         = false;
            break;
        case VK_UP:
        case 'W':
            fullView.centerZ -= step;
            following         = false;
            break;
        case VK_DOWN:
        case 'S':
            fullView.centerZ += step;
            following         = false;
            break;
        default:
            break;
        }
    }
    void mouse(ll::event::MouseInputEvent& event) {
        if (transitioning) return;
        int button = static_cast<unsigned char>(event.actionButtonId());
        int data   = event.buttonData();
        if (!fullscreen) {
            if (data == 0 && consumedMouse.erase(button)) event.cancel();
            return;
        }
        if (!client || !hud(*client) || !foreground()) {
            closeMap(false);
            return;
        }
        // Cancelling the raw event stops the engine cursor from being updated.
        // Own the pointer position, using the same coordinates for drawing and hit testing.
        if (!pollPointer()) movePointer(event.x() * mouseScaleX, event.y() * mouseScaleY);
        float x = mouseX, y = mouseY;
        // Bedrock MouseDevice: 0=motion, 1=left, 2=right, 3=middle, 4=wheel.
        if (button == 1) dragging = data != 0 && area.contains(x, y);
        if (button == 4 && data != 0 && area.contains(x, y)) zoom(data > 0 ? 0.8 : 1.25);
        if (button >= 1 && button <= 3) {
            if (data != 0) consumedMouse.insert(button);
            else consumedMouse.erase(button);
        }
        event.cancel();
    }
    void movePointer(float x, float y) {
        if (dragging && hasMouse && area.width > 0 && area.height > 0) {
            fullView.centerX -= (x - mouseX) / area.width * fullView.width * fullView.blocksPerPixel;
            fullView.centerZ -= (y - mouseY) / area.height * fullView.height * fullView.blocksPerPixel;
            following         = false;
        }
        mouseX   = x;
        mouseY   = y;
        hasMouse = true;
    }
    bool pollPointer() {
        if (guiWidth <= 0 || guiHeight <= 0 || !foreground()) return false;
        POINT point{};
        RECT  bounds{};
        auto  window = GetForegroundWindow();
        if (!GetCursorPos(&point) || !ScreenToClient(window, &point) || !GetClientRect(window, &bounds)
            || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
            return false;
        movePointer(
            static_cast<float>(point.x - bounds.left) * guiWidth / (bounds.right - bounds.left),
            static_cast<float>(point.y - bounds.top) * guiHeight / (bounds.bottom - bounds.top)
        );
        return true;
    }
    void render(ll::event::AfterUIRenderEvent& event) {
        auto& ctx = event.uiRenderContext();
        auto& ci  = ctx.mClient;
        if (!ci.isPrimaryClient()) return;
        if (resetTexture) {
            if (renderer) renderer->reset();
            resetTexture = false;
        }
        if (!client || client != &ci || !ci.getLocalPlayer()) return;
        if (fullscreen && (!foreground() || !hud(ci))) closeMap(false);
        if (renderFailed || event.screenView().getScreenName() != "hud_screen" || !hud(ci)) return;
        if (!fullscreen && !settings.showMinimap) return;
        auto        gui   = ci.getGuiData();
        auto const& size  = gui->mScreenSizeData;
        float       width = size->clientUIScreenSize->x, height = size->clientUIScreenSize->y;
        if (width < 96 || height < 96) return;
        if (fullscreen && (guiWidth != width || guiHeight != height)) {
            dragging = false;
            hasMouse = false;
        }
        guiWidth    = width;
        guiHeight   = height;
        mouseScaleX = size->clientScreenSize->x > 0 ? width / size->clientScreenSize->x : 1.0f;
        mouseScaleY = size->clientScreenSize->y > 0 ? height / size->clientScreenSize->y : 1.0f;
        MapView view;
        if (fullscreen) {
            // The native HUD may recapture the mouse after a focus/input-mode change.
            if (ci.getMouseGrabbed()) {
                InputTransition transition(transitioning);
                ci.releaseMouse();
                dragging = false;
                hasMouse = false;
            }
            pollPointer();
            if (!hasMouse) movePointer(width * 0.5f, height * 0.5f);
            area            = {14, 26, width - 28, height - 68};
            fullView.width  = settings.fullscreenPixels;
            fullView.height = std::clamp(static_cast<int>(fullView.width * area.height / area.width), 64, 768);
            if (following) {
                fullView.centerX = playerX;
                fullView.centerZ = playerZ;
            }
            fullView.centerX = std::clamp(fullView.centerX, -29999000.0, 29999000.0);
            fullView.centerZ = std::clamp(fullView.centerZ, -29999000.0, 29999000.0);
            view             = fullView;
        } else {
            float side = std::min({settings.minimapSize, width * 0.40f, height - 40.0f});
            area       = {width - side - 9, 20, side, side};
            view = {playerX, playerZ, settings.minimapBlocksPerPixel, settings.minimapPixels, settings.minimapPixels};
        }
        // Snap texture origin to a texel; the player marker remains smooth every frame.
        view.centerX = std::floor(view.centerX / view.blocksPerPixel) * view.blocksPerPixel;
        view.centerZ = std::floor(view.centerZ / view.blocksPerPixel) * view.blocksPerPixel;
        bool urgent  = pixels.empty() || renderedView.width != view.width || renderedView.height != view.height
                    || rasterDimension != dimension;
        if (urgent
            || (Clock::now() - lastRaster >= std::chrono::milliseconds(settings.refreshMilliseconds)
                && (renderedView != view || rasterRevision != cache.revision()))) {
            pixels          = cache.rasterize(dimension, view);
            renderedView    = view;
            rasterRevision  = cache.revision();
            rasterDimension = dimension;
            lastRaster      = Clock::now();
            ++imageRevision;
        }
        if (!renderer) renderer = std::make_unique<MapRenderer>();
        auto caption = dimension == 0 ? fmt::format("X {:.0f}  Y {:.0f}  Z {:.0f}", playerX, playerY, playerZ)
                                      : std::string("Surface map: Overworld only");
        try {
            renderer
                ->render(ctx, area, renderedView, pixels, imageRevision, playerX, playerZ, yaw, caption, fullscreen);
            if (fullscreen && hasMouse && mouseX >= 0 && mouseY >= 0 && mouseX < width && mouseY < height)
                renderer->renderCursor(ctx, mouseX, mouseY, dragging);
        } catch (std::exception const& ex) {
            renderFailed = true;
            closeMap(true);
            mod.getLogger().error("Wayfinder renderer disabled for this session: {}", ex.what());
        }
    }
};

Wayfinder::Wayfinder(ll::mod::NativeMod& mod) : mImpl(std::make_unique<Impl>(mod)) {}
Wayfinder::~Wayfinder() { disable(); }
bool Wayfinder::enable() {
    std::unique_lock lock(mImpl->mutex);
    if (mImpl->enabled) return true;
    mImpl->enabled = true;
    try {
        auto weak = weak_from_this();
        mImpl->subscribe<ll::event::ClientLevelTickEvent>(weak, &Impl::tick);
        mImpl->subscribe<ll::event::BlockChangedEvent>(weak, &Impl::changed);
        mImpl->subscribe<ll::event::ClientExitLevelEvent>(weak, &Impl::exit);
        mImpl->subscribe<ll::event::KeyInputEvent>(weak, &Impl::key);
        mImpl->subscribe<ll::event::MouseInputEvent>(weak, &Impl::mouse);
        mImpl->subscribe<ll::event::AfterUIRenderEvent>(weak, &Impl::render);
        mImpl->mod.getLogger().info(
            "Wayfinder enabled. World map key: {}; minimap key: {} (Windows VK codes).",
            mImpl->settings.fullMapKey,
            mImpl->settings.toggleMinimapKey
        );
        return true;
    } catch (std::exception const& ex) {
        mImpl->mod.getLogger().error("Wayfinder enable failed: {}", ex.what());
        lock.unlock();
        disable();
        return false;
    }
}
void Wayfinder::disable() {
    std::vector<ll::event::ListenerPtr> listeners;
    {
        std::lock_guard lock(mImpl->mutex);
        if (!mImpl->enabled) return;
        mImpl->enabled = false;
        listeners.swap(mImpl->listeners);
    }
    // Removing hooks must not wait for callbacks while holding the callback mutex.
    for (auto const& listener : listeners) ll::event::EventBus::getInstance().removeListener(listener);
    std::lock_guard lock(mImpl->mutex);
    try {
        mImpl->closeMap(true);
        mImpl->endSession();
    } catch (std::exception const& ex) {
        mImpl->mod.getLogger().error("Wayfinder shutdown: {}", ex.what());
        mImpl->client = nullptr;
    }
    mImpl->finishSave();
    mImpl->renderer.reset();
}
} // namespace wayfinder
