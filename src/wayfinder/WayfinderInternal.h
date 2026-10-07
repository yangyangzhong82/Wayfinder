#pragma once
// Private composition and cross-component contracts; not part of the mod API.
#include "wayfinder/Wayfinder.h"
#include "wayfinder/ExplorationTrail.h"
#include "wayfinder/EntityRadar.h"
#include "wayfinder/MapHistory.h"
#include "wayfinder/MapMenu.h"
#include "wayfinder/NativeTextInput.h"
#include "wayfinder/MapMouse.h"
#include "wayfinder/MapInspection.h"
#include "wayfinder/WayfinderRendering.h"
#include "wayfinder/RenderRecovery.h"
#include "wayfinder/TerrainSampler.h"
#include "wayfinder/WayfinderView.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/event/world/BlockChangedEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include <Windows.h>
#include <chrono>
#include <future>
#include <mutex>
#include <unordered_set>

namespace wayfinder {
struct Wayfinder::Impl {
    using Clock = std::chrono::steady_clock;

    struct Session {
        struct Player {
            int dimension{};
            double x{}, y{}, z{};
            float yaw{};
            std::string biome;
            Clock::time_point lastBiome{};
        } player;
        CursorBiome cursorBiome;
        Clock::time_point lastCursorBiome{};
        MapLayer layer;
        int overworldSkyDarken{};
        std::vector<EntityMarker> entities;
        Clock::time_point lastEntities{};

        MapHistory history;

        MapCache cache;
        TerrainSampler sampler;
        IClientInstance* client{}; // Cleared on exit; never captured by a worker.
        Navigation navigation;
        ExplorationTrail trail;
        struct TrailSave { std::uint64_t revision{}; std::string error; };
        std::future<TrailSave> trailSaveTask;
        std::filesystem::path trailPath;
        std::uint64_t savedTrailRevision{};
        Clock::time_point lastTrailSave{};
        bool trailSaveFailed{};
        DeathTracker deathTracker;
        std::filesystem::path navigationPath;
        std::string identity;
        bool navigationDirty{};
        Clock::time_point lastNavigationAttempt{};

        explicit Session(std::size_t capacity) : cache(capacity) {}
        void changed(ll::event::BlockChangedEvent& event);
    };

    struct Input {
        bool restoreMouse{}, transitioning{}, dragging{}, hasMouse{};
        float mouseX{}, mouseY{};
        std::uint64_t pressedMarker{};
        float pressX{}, pressY{};
        HWND mapWindow{};
        std::unique_ptr<MapMouse> mapMouse;
        std::unordered_set<int> heldKeys, consumedKeys, consumedMouse, panKeys;
        Clock::time_point lastPan{};

        void stopGesture() { pressedMarker = 0; panKeys.clear(); dragging = false; }
        void clearKeys() { heldKeys.clear(); consumedKeys.clear(); consumedMouse.clear(); panKeys.clear(); }
        void movePointer(WayfinderView& view, bool mapPage, float x, float y);
        bool pollPointer(WayfinderView& view, bool mapPage);
        void panHeldKeys(WayfinderView& view);
    };

    struct Menu {
        MapMenu state;
        UiFrame frame;
        std::string gameLanguage{"en_US"};
        NativeTextInput textInput;
        UiAction nativeField{UiAction::None};
        MapMenu::Page nativePage{MapMenu::Page::Map};

        void refreshLocale(Settings const& settings);
    };

    ll::mod::NativeMod& mod;
    Settings settings;
    // Guards outlive the components, including partial enable/constructor cleanup.
    std::recursive_mutex mutex;
    std::vector<ll::event::ListenerPtr> listeners;
    bool enabled{};
    Clock::time_point lastError{};
    Session session;
    Input input;
    WayfinderView view;
    Menu ui;
    WayfinderRendering rendering;
    RenderRecovery renderRecovery;

    // Cross-component workflows live here. Components receive only their explicit
    // inputs; they never receive Impl or reach back into the enclosing runtime.
    void beginSession(IClientInstance& client);
    void endSession();
    void tick(ll::event::ClientLevelTickEvent& event);
    void exit(ll::event::ClientExitLevelEvent& event);
    void persistNavigation();
    void persistTrail(bool force);
    void sampleEntities();
    void collectHistory(bool force = false);
    void saveHistory(bool force = false);
    void applyHistory(MapHistory::Result result);
    void invalidateDisplayedMap();
    void followPlayer();
    void selectDisplayedLayer(MapLayer layer);
    void changeDimension(int dimension);
    void changeLiveLayer(MapLayer layer);
    void fitKnownArea();
    void openMap();
    void closeMap(bool grab);
    void key(ll::event::KeyInputEvent& event);
    void mouse(ll::event::MouseInputEvent& event);
    void newWaypoint(bool atPointer);
    void openContext();
    void teleport();
    void menuAction(UiButton const& button);
    void menuKey(int code, bool first);
    void refreshExploration();
    void openText();
    void pollText();
    void render(ll::event::AfterUIRenderEvent& event);
    bool drawFrame(ll::event::AfterUIRenderEvent& event);
    void notifyRender(std::string_view message);

    explicit Impl(ll::mod::NativeMod& owner);
    static bool foreground();
    static bool hud(IClientInstance& client);
    static std::int64_t unixTime();
    template <class Function> void guarded(Function&& function);
    template <class Event, class Function> void subscribe(std::weak_ptr<Wayfinder> weak, Function handler);
};
} // namespace wayfinder
