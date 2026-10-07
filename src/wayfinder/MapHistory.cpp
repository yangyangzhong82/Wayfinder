#include "wayfinder/MapHistory.h"
#include <utility>

namespace wayfinder {
void MapHistory::start(std::filesystem::path path, std::string identity, std::size_t capacity) {
    reset();
    savePath = path;
    lastSave = Clock::now();
    progress = std::make_shared<ArchiveLoadProgress>();
    rejected = 0;
    loadFailed = recovered = false;
    auto state = progress;
    openTask = std::async(std::launch::async, [path, identity, capacity, state] {
        OpenJobResult result;
        result.path = path;
        try {
            try {
                result.archive = std::make_shared<MapArchive>(path, identity, capacity, state);
            } catch (std::exception const& ex) {
                result.error = ex.what();
                result.path += ".recovery-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
                result.archive = std::make_shared<MapArchive>(result.path, identity, capacity, state);
            }
            result.recent = result.archive->recent(capacity);
        } catch (std::exception const& ex) {
            result.archive.reset();
            result.error += std::string("; ") + ex.what();
        }
        return result;
    });
}
std::optional<MapHistory::OpenResult> MapHistory::finishOpen(bool force) {
    if (!openTask.valid() || (!force && openTask.wait_for(std::chrono::seconds(0)) != std::future_status::ready)) return {};
    auto job = openTask.get();
    recovered = job.path != savePath;
    savePath = std::move(job.path);
    archive = std::move(job.archive);
    loadFailed = !archive;
    OpenResult result;
    result.recent = std::move(job.recent);
    result.error = std::move(job.error);
    result.loaded = bool(archive);
    if (archive) {
        bounds = archive->bounds();
        rejected = archive->rejectedTiles();
        result.warnings = archive->takeWarnings();
        result.tiles = archive->size();
        result.rejected = rejected;
    }
    progress.reset();
    if (!archive) requestedPoint.reset();
    return result;
}
std::optional<MapHistory::Result> MapHistory::finish(bool force) {
    if (!saveTask.valid() || (!force && saveTask.wait_for(std::chrono::seconds(0)) != std::future_status::ready)) return {};
    auto result = saveTask.get();
    rejected = result.rejected;
    if (!result.error.empty()) {
        retryChanges.insert(retryChanges.end(), inFlightChanges->begin(), inFlightChanges->end());
        lastError = Clock::now();
    } else {
        lastError = {};
        bounds = std::move(result.bounds);
    }
    inFlightChanges.reset();
    return result;
}
void MapHistory::clearRequests() {
    requestedView.reset();
    requestedPoint.reset();
    requestedBiome.reset();
}
void MapHistory::reset() {
    // Session shutdown must collect results before releasing this archive.
    if (openTask.valid() || saveTask.valid()) throw std::logic_error("History jobs must be drained before reset");
    archive.reset();
    progress.reset();
    inFlightChanges.reset();
    retryChanges.clear();
    clearRequests();
    bounds.clear();
    lastError = {};
    loadFailed = recovered = false;
    rejected = 0;
}
bool MapHistory::samplingAllowed() const {
    return retryChanges.empty() && Clock::now() - lastError >= std::chrono::seconds(5);
}
std::string MapHistory::status(Locale const& locale) const {
    if (loadFailed) return locale.tr("History unavailable; live map only");
    if (openTask.valid() && progress) {
        auto phase = progress->phase.load();
        if (phase == ArchiveLoadProgress::Phase::Scanning)
            return locale.tr("Scanning history: ") + std::to_string(progress->total.load());
        if (phase == ArchiveLoadProgress::Phase::Migrating) return locale.tr("Migrating history...");
        if (phase == ArchiveLoadProgress::Phase::Ready) return locale.tr("Preparing history...");
        return locale.tr("Loading history: ") + std::to_string(progress->completed.load()) + "/"
            + std::to_string(progress->total.load());
    }
    if (lastError != Clock::time_point{}) return locale.tr("History save failed; retrying");
    if (rejected) return locale.tr("History tiles isolated: ") + std::to_string(rejected);
    if (recovered) return locale.tr("History recovery archive active");
    return {};
}
bool MapHistory::canSave(bool force) const {
    return archive && !openTask.valid() && !saveTask.valid()
        && (force || Clock::now() - lastError >= std::chrono::seconds(5));
}
void MapHistory::save(std::vector<TileRecord> changes, std::uint64_t revision, SaveOptions options, bool force) {
    // Take ownership even if called while busy; submitted changes must not vanish.
    retryChanges.insert(retryChanges.end(), std::make_move_iterator(changes.begin()), std::make_move_iterator(changes.end()));
    if (!canSave(force)) return;
    auto records = std::move(retryChanges);
    retryChanges.clear();
    bool flush = force || Clock::now() - lastSave >= std::chrono::seconds(options.autosaveSeconds);
    if (records.empty() && !requestedView && !requestedPoint && !requestedBiome && !flush) return;
    auto history = archive;
    auto request = std::exchange(requestedView, {});
    auto point = std::exchange(requestedPoint, {});
    auto biomePoint = std::exchange(requestedBiome, {});
    bool biomeRegions = options.biomeRegions;
    auto currentLayer = requestedLayer;
    MapLighting lighting{options.lighting, currentLayer.dimension == 0 ? options.overworldSkyDarken : 0};
    inFlightChanges = std::make_shared<std::vector<TileRecord>>(std::move(records));
    auto batch = inFlightChanges;
    try {
        saveTask = std::async(std::launch::async, [history, batch, request, point, biomePoint, biomeRegions, lighting, currentLayer, revision, flush] {
            Result result;
            result.view = request;
            result.layer = currentLayer;
            result.revision = revision;
            result.point = point;
            result.biomePoint = biomePoint;
            result.biomeRegions = biomeRegions;
            result.lighting = lighting;
            try {
                history->update(*batch);
                if (point) result.pointCell = history->get(point->layer, point->x, point->z);
                if (biomePoint) result.pointBiome = history->biome(biomePoint->layer, biomePoint->x, biomePoint->z);
                if (request) result.pixels = history->rasterize(currentLayer, *request, lighting);
                if (request && biomeRegions) result.biomes = history->biomes(currentLayer, *request);
                if (flush) history->flush();
                result.bounds = history->bounds();
            } catch (std::exception const& ex) {
                result.error = ex.what();
            }
            result.rejected = history->rejectedTiles();
            result.warnings = history->takeWarnings();
            return result;
        });
    } catch (...) {
        retryChanges = std::move(*inFlightChanges);
        inFlightChanges.reset();
        requestedView = request;
        requestedPoint = point;
        requestedBiome = biomePoint;
        throw;
    }
    if (flush) lastSave = Clock::now();
}

} // namespace wayfinder
