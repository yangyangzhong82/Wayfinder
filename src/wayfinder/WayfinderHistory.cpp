#include "wayfinder/WayfinderInternal.h"
#include "ll/api/mod/NativeMod.h"
#include <utility>

namespace wayfinder {
void Wayfinder::Impl::Session::History::start(Impl& app) {
    progress = std::make_shared<ArchiveLoadProgress>();
    rejected = 0;
    loadFailed = recovered = false;
    auto path = savePath;
    auto identity = app.session.identity;
    auto capacity = app.session.cache.capacity();
    auto state = progress;
    openTask = std::async(std::launch::async, [path, identity, capacity, state] {
        OpenResult result;
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
void Wayfinder::Impl::Session::History::finishOpen(Impl& app) {
    if (!openTask.valid()) return;
    auto result = openTask.get();
    recovered = result.path != savePath;
    savePath = std::move(result.path);
    archive = std::move(result.archive);
    loadFailed = !archive;
    if (!result.error.empty()) app.mod.getLogger().warn("Wayfinder history open: {}", result.error);
    if (archive) {
        app.session.cache.mergeHistory(result.recent);
        bounds = archive->bounds();
        rejected = archive->rejectedTiles();
        for (auto const& warning : archive->takeWarnings()) app.mod.getLogger().warn("Wayfinder history: {}", warning);
        app.mod.getLogger().info("Wayfinder history ready: {} ({} tiles, {} isolated)", savePath.string(), archive->size(), rejected);
        app.rendering.lastRaster = {};
    }
    progress.reset();
    if (!archive && requestedPoint) {
        requestedPoint.reset();
        app.ui.state.contextLoading = false;
    }
}
std::string Wayfinder::Impl::Session::History::status(Locale const& locale) const {
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
void Wayfinder::Impl::Session::History::finish(Impl& app) {
    if (!saveTask.valid()) return;
    auto result = saveTask.get();
    auto& menu = app.ui.state;
    if (result.biomePoint && app.session.cursorBiome.point == result.biomePoint && result.error.empty()) {
        auto& cursor = app.session.cursorBiome;
        cursor.historyRead = true;
        if (cursor.identifier.empty()) cursor.identifier = std::move(result.pointBiome);
    }
    if (result.point && menu.page == MapMenu::Page::Context && menu.contextLoading
        && app.view.displayedLayer(app.session.layer) == result.point->layer
        && menu.draft.dimension == result.point->layer.dimension && menu.draft.x == result.point->x
        && menu.draft.z == result.point->z) {
        auto cell = app.session.cache.get(result.point->layer, result.point->x, result.point->z);
        if (!cell.known()) cell = result.pointCell;
        menu.contextLoading = false;
        if (cell.floor()) menu.draft.y = cell.height;
        else if (!result.error.empty()) menu.fail("Could not read terrain height; try again.");
    }
    rejected = result.rejected;
    for (auto const& warning : result.warnings) app.mod.getLogger().warn("Wayfinder history: {}", warning);
    if (!result.error.empty()) {
        app.mod.getLogger().error("Wayfinder history: {}", result.error);
        retryChanges.insert(retryChanges.end(), inFlightChanges->begin(), inFlightChanges->end());
        lastError = Clock::now();
        // Retry the image after the I/O error; do not mark stale pixels current.
        app.rendering.lastRaster = {};
    } else {
        lastError = {};
        bounds = std::move(result.bounds);
        if (result.view && result.layer == app.view.displayedLayer(app.session.layer)
            && result.biomeRegions == app.settings.showBiomeRegions
            && result.lighting == MapLighting{app.settings.showLighting, result.layer.dimension == 0 ? app.session.overworldSkyDarken : 0}) {
            auto visible = app.view.fullscreen
                ? app.view.fullView
                : MapView{app.session.player.x, app.session.player.z, app.settings.minimapBlocksPerPixel,
                          app.settings.minimapPixels, app.settings.minimapPixels};
            auto expected = detailTextureView(app.view.fullscreen ? visible : minimapTextureView(visible),
                                              app.settings.terrainPixelScale);
            // A completed job may belong to an old map mode, scale, or player
            // position. Keep its archive writes, but never roll the minimap back
            // to a texture that cannot cover its current visible rectangle.
            if (result.view->width == expected.width && result.view->height == expected.height
                && result.view->blocksPerPixel == expected.blocksPerPixel
                && (app.view.fullscreen || textureCoversView(*result.view, visible, app.settings.terrainPixelScale))) {
                app.rendering.pixels = std::move(result.pixels);
                app.rendering.biomes = std::move(result.biomes);
                app.rendering.rasterBiomes = result.biomeRegions;
                app.rendering.rasterLighting = result.lighting;
                app.rendering.textureView = *result.view;
                app.rendering.renderedView = app.view.fullscreen ? *result.view : visible;
                app.rendering.rasterLayer = result.layer;
                app.rendering.rasterRevision = result.revision;
                ++app.rendering.imageRevision;
            }
        }
    }
    inFlightChanges.reset();
}

void Wayfinder::Impl::Session::History::save(Impl& app, bool force) {
    if (openTask.valid()) {
        if (!force && openTask.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        finishOpen(app);
    }
    if (!archive) return;
    if (saveTask.valid()) {
        if (!force && saveTask.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        finish(app);
    }
    if (!force && Clock::now() - lastError < std::chrono::seconds(5)) return;
    auto records = std::move(retryChanges);
    retryChanges.clear();
    auto changes = app.session.cache.takeChanges();
    records.insert(records.end(), changes.begin(), changes.end());
    bool flush = force || Clock::now() - lastSave >= std::chrono::seconds(app.settings.autosaveSeconds);
    if (records.empty() && !requestedView && !requestedPoint && !requestedBiome && !flush) return;
    auto history = archive;
    auto request = std::exchange(requestedView, {});
    auto point = std::exchange(requestedPoint, {});
    auto biomePoint = std::exchange(requestedBiome, {});
    bool biomeRegions = app.settings.showBiomeRegions;
    auto currentLayer = requestedLayer;
    MapLighting lighting{app.settings.showLighting, currentLayer.dimension == 0 ? app.session.overworldSkyDarken : 0};
    auto revision = app.session.cache.revision();
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
    if (force) finish(app);
}

bool Wayfinder::Impl::Session::History::ready() const {
    return saveTask.valid() && saveTask.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}
} // namespace wayfinder
