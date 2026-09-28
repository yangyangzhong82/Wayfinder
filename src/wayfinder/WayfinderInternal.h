#pragma once
// Private composition and cross-component contracts; not part of the mod API.
#include "wayfinder/Wayfinder.h"
#include "wayfinder/EntityRadar.h"
#include "wayfinder/MapArchive.h"
#include "wayfinder/MapMenu.h"
#include "wayfinder/MapMouse.h"
#include "wayfinder/MapRenderer.h"
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
        MapLayer layer;
        std::vector<EntityMarker> entities;
        Clock::time_point lastEntities{};

        // One serial worker owns archive access. Captures are values/shared data only,
        // never Impl, Session, an engine object, or a UI/renderer reference.
        struct History {
            struct PointRequest {
                MapLayer layer;
                int x{}, z{};
            };
            struct Result {
                std::optional<MapView> view;
                MapLayer layer;
                std::uint64_t revision{};
                std::vector<std::uint32_t> pixels;
                std::optional<PointRequest> point;
                MapCell pointCell;
                std::unordered_map<MapLayer, MapBounds, MapLayerHash> bounds;
                std::string error;
                std::vector<std::string> warnings;
                std::size_t rejected{};
            };
            struct OpenResult {
                std::shared_ptr<MapArchive> archive;
                std::vector<TileRecord> recent;
                std::filesystem::path path;
                std::string error;
            };
            std::shared_ptr<MapArchive> archive;
            std::shared_ptr<std::vector<TileRecord>> inFlightChanges;
            std::vector<TileRecord> retryChanges;
            std::optional<MapView> requestedView;
            std::optional<PointRequest> requestedPoint;
            std::unordered_map<MapLayer, MapBounds, MapLayerHash> bounds;
            std::filesystem::path savePath;
            std::future<Result> saveTask;
            std::future<OpenResult> openTask;
            std::shared_ptr<ArchiveLoadProgress> progress;
            std::size_t rejected{};
            bool loadFailed{}, recovered{};
            Clock::time_point lastSave{}, lastError{};

            void start(Impl& app);
            void finishOpen(Impl& app);
            std::string status(Locale const& locale) const;
            void finish(Impl& app);
            void save(Impl& app, bool force);
            bool ready() const;
        } history;

        MapCache cache;
        TerrainSampler sampler;
        IClientInstance* client{}; // Cleared on exit; never captured by a worker.
        Navigation navigation;
        DeathTracker deathTracker;
        std::filesystem::path navigationPath;
        std::string identity;
        bool navigationDirty{};
        Clock::time_point lastNavigationAttempt{};

        explicit Session(std::size_t capacity) : cache(capacity) {}
        void begin(Impl& app, IClientInstance& ci);
        void end(Impl& app);
        void persistNavigation(Impl& app);
        void sampleEntities(Impl& app);
        void tick(Impl& app, ll::event::ClientLevelTickEvent& event);
        void changed(ll::event::BlockChangedEvent& event);
        void exit(Impl& app, ll::event::ClientExitLevelEvent& event);
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

        void openMap(Impl& app);
        void closeMap(Impl& app, bool grab);
        void followPlayer(Impl& app);
        void key(Impl& app, ll::event::KeyInputEvent& event);
        void mouse(Impl& app, ll::event::MouseInputEvent& event);
        void movePointer(Impl& app, float x, float y);
        bool pollPointer(Impl& app);
        void panHeldKeys(Impl& app);
    };

    struct Menu {
        MapMenu state;
        UiFrame frame;
        std::string gameLanguage{"en_US"};

        void refreshLocale(Settings const& settings);
        void newWaypoint(Impl& app, bool atPointer);
        void openContext(Impl& app);
        void teleport(Impl& app);
        void action(Impl& app, UiButton const& button);
        void key(Impl& app, int code, bool first);
    };

    struct Rendering {
        std::unique_ptr<MapRenderer> renderer;
        bool resetTexture{}, failed{};
        MapView renderedView; // Visible world rectangle shared by overlays and pointer projection.
        MapView textureView;  // World rectangle of pixels, including the minimap border.
        std::vector<std::uint32_t> pixels;
        std::uint64_t imageRevision{}, rasterRevision{};
        MapLayer rasterLayer{std::numeric_limits<int>::min()};
        Clock::time_point lastRaster{};

        void refreshImage(Impl& app, MapView const& view);
        void render(Impl& app, ll::event::AfterUIRenderEvent& event);
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
    Rendering rendering;

    explicit Impl(ll::mod::NativeMod& owner);
    static bool foreground();
    static bool hud(IClientInstance& client);
    static std::int64_t unixTime();
    template <class Function> void guarded(Function&& function);
    template <class Event, class Function> void subscribe(std::weak_ptr<Wayfinder> weak, Function handler);
};
} // namespace wayfinder
