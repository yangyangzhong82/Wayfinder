#include "wayfinder/WayfinderInternal.h"
#include "ll/api/mod/NativeMod.h"

namespace wayfinder {
void Wayfinder::Impl::Session::persistTrail(Impl& app, bool force) {
    auto finish = [&] {
        auto result = trailSaveTask.get();
        trailSaveFailed = !result.error.empty();
        if (!trailSaveFailed) savedTrailRevision = result.revision;
        else app.mod.getLogger().warn("Wayfinder trail save: {}", result.error);
    };
    if (trailSaveTask.valid()) {
        if (!force && trailSaveTask.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        finish();
    }
    if (identity.empty() || savedTrailRevision == trail.revision) return;
    auto interval = std::chrono::seconds(trailSaveFailed ? 5 : app.settings.autosaveSeconds);
    if (!force && Clock::now() - lastTrailSave < interval) return;
    lastTrailSave = Clock::now();
    auto path = trailPath;
    auto world = identity;
    auto points = trail.points;
    auto revision = trail.revision;
    try {
        trailSaveTask = std::async(std::launch::async, [path, world, points = std::move(points), revision] {
            TrailSave result{revision, {}};
            try { writeTrail(path, world, points); }
            catch (std::exception const& ex) { result.error = ex.what(); }
            return result;
        });
        if (force) finish();
    } catch (std::exception const& ex) {
        trailSaveFailed = true;
        app.mod.getLogger().warn("Wayfinder trail worker: {}", ex.what());
    }
}
void Wayfinder::Impl::Menu::refreshExploration(Impl& app) {
    state.displayedLayer = app.view.displayedLayer(app.session.layer);
    state.layerLocked = app.view.fullscreen && app.view.lockedLayer.has_value();
    state.trailPoints = app.session.trail.points.size();
    state.retracing = app.session.trail.retraceIndex().has_value();
    state.trailSaveFailed = app.session.trailSaveFailed;
    auto add = [&](MapLayer layer) {
        if (std::find(state.knownLayers.begin(), state.knownLayers.end(), layer) == state.knownLayers.end())
            state.knownLayers.push_back(layer);
    };
    add(app.session.layer);
    add(state.displayedLayer);
    for (auto const& [layer, bounds] : app.session.history.bounds) add(layer);
    std::sort(state.knownLayers.begin(), state.knownLayers.end(), [](MapLayer a, MapLayer b) {
        return a.dimension != b.dimension ? a.dimension < b.dimension : a.slice > b.slice;
    });
}
} // namespace wayfinder
