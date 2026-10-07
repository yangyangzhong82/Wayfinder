#include "wayfinder/WayfinderInternal.h"
#include "ll/api/mod/NativeMod.h"

namespace wayfinder {
void Wayfinder::Impl::persistTrail(bool force) {
    auto finish = [&] {
        auto result = session.trailSaveTask.get();
        session.trailSaveFailed = !result.error.empty();
        if (!session.trailSaveFailed) session.savedTrailRevision = result.revision;
        else mod.getLogger().warn("Wayfinder trail save: {}", result.error);
    };
    if (session.trailSaveTask.valid()) {
        if (!force && session.trailSaveTask.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        finish();
    }
    if (session.identity.empty() || session.savedTrailRevision == session.trail.revision) return;
    auto interval = std::chrono::seconds(session.trailSaveFailed ? 5 : settings.autosaveSeconds);
    if (!force && Clock::now() - session.lastTrailSave < interval) return;
    session.lastTrailSave = Clock::now();
    auto path = session.trailPath;
    auto world = session.identity;
    auto points = session.trail.points;
    auto revision = session.trail.revision;
    try {
        session.trailSaveTask = std::async(std::launch::async, [path, world, points = std::move(points), revision] {
            Session::TrailSave result{revision, {}};
            try { writeTrail(path, world, points); }
            catch (std::exception const& ex) { result.error = ex.what(); }
            return result;
        });
        if (force) finish();
    } catch (std::exception const& ex) {
        session.trailSaveFailed = true;
        mod.getLogger().warn("Wayfinder trail worker: {}", ex.what());
    }
}
void Wayfinder::Impl::refreshExploration() {
    ui.state.displayedLayer = view.displayedLayer(session.layer);
    ui.state.layerLocked = view.fullscreen && view.lockedLayer.has_value();
    ui.state.trailPoints = session.trail.points.size();
    ui.state.retracing = session.trail.retraceIndex().has_value();
    ui.state.trailSaveFailed = session.trailSaveFailed;
    auto add = [&](MapLayer layer) {
        if (std::find(ui.state.knownLayers.begin(), ui.state.knownLayers.end(), layer) == ui.state.knownLayers.end())
            ui.state.knownLayers.push_back(layer);
    };
    add(session.layer);
    add(ui.state.displayedLayer);
    for (auto const& [layer, bounds] : session.history.layerBounds()) add(layer);
    std::sort(ui.state.knownLayers.begin(), ui.state.knownLayers.end(), [](MapLayer a, MapLayer b) {
        return a.dimension != b.dimension ? a.dimension < b.dimension : a.slice > b.slice;
    });
}
} // namespace wayfinder
