#pragma once
#include "wayfinder/MapArchive.h"
#include "wayfinder/MapInspection.h"
#include "wayfinder/Locale.h"
#include <chrono>
#include <future>

namespace wayfinder {
// Owns archive jobs and retries. Results are applied by the coordinator; this
// component has no access to the session, UI, renderer, or engine objects.
class MapHistory {
public:
    using Clock = std::chrono::steady_clock;
    using PointRequest = MapInspectionPoint;
    struct Result {
        std::optional<MapView> view;
        MapLayer layer;
        std::uint64_t revision{};
        std::vector<std::uint32_t> pixels;
        BiomeMap biomes;
        bool biomeRegions{};
        MapLighting lighting;
        std::optional<MapInspectionPoint> biomePoint;
        std::string pointBiome;
        std::optional<PointRequest> point;
        MapCell pointCell;
        std::unordered_map<MapLayer, MapBounds, MapLayerHash> bounds;
        std::string error;
        std::vector<std::string> warnings;
        std::size_t rejected{};
    };
    struct OpenResult {
        std::vector<TileRecord> recent;
        std::string error;
        std::vector<std::string> warnings;
        std::size_t tiles{}, rejected{};
        bool loaded{};
    };
    struct SaveOptions {
        int autosaveSeconds{};
        bool biomeRegions{}, lighting{};
        int overworldSkyDarken{};
    };

    void start(std::filesystem::path path, std::string identity, std::size_t capacity);
    std::optional<OpenResult> finishOpen(bool force = false);
    std::optional<Result> finish(bool force = false);
    bool canSave(bool force = false) const;
    void save(std::vector<TileRecord> changes, std::uint64_t revision, SaveOptions options, bool force = false);
    void reset(); // Caller drains jobs before ending the session.
    void requestView(MapView view, MapLayer layer) { requestedView = view; requestedLayer = layer; }
    void requestPoint(PointRequest point) { requestedPoint = point; }
    void requestBiome(MapInspectionPoint point) { requestedBiome = point; }
    void cancelPoint() { requestedPoint.reset(); }
    void clearRequests();
    bool available() const { return archive || openTask.valid(); }
    bool liveOnly() const { return !archive; }
    bool needsRetry() const { return !retryChanges.empty(); }
    bool samplingAllowed() const;
    std::filesystem::path const& path() const { return savePath; }
    auto const& layerBounds() const { return bounds; }
    std::string status(Locale const& locale) const;

private:
    struct OpenJobResult {
        std::shared_ptr<MapArchive> archive;
        std::vector<TileRecord> recent;
        std::filesystem::path path;
        std::string error;
    };
    std::shared_ptr<MapArchive> archive;
    std::shared_ptr<std::vector<TileRecord>> inFlightChanges;
    std::vector<TileRecord> retryChanges;
    std::optional<MapView> requestedView;
    MapLayer requestedLayer;
    std::optional<PointRequest> requestedPoint;
    std::optional<MapInspectionPoint> requestedBiome;
    std::unordered_map<MapLayer, MapBounds, MapLayerHash> bounds;
    std::filesystem::path savePath;
    std::future<Result> saveTask;
    std::future<OpenJobResult> openTask;
    std::shared_ptr<ArchiveLoadProgress> progress;
    std::size_t rejected{};
    bool loadFailed{}, recovered{};
    Clock::time_point lastSave{}, lastError{};
};
} // namespace wayfinder
