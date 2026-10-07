#include "wayfinder/WayfinderInternal.h"
#include "ll/api/mod/NativeMod.h"

namespace wayfinder {
void Wayfinder::Impl::collectHistory(bool force) {
    if (auto result = session.history.finishOpen(force)) {
        if (!result->error.empty()) mod.getLogger().warn("Wayfinder history open: {}", result->error);
        if (result->loaded) {
            session.cache.mergeHistory(result->recent);
            for (auto const& warning : result->warnings) mod.getLogger().warn("Wayfinder history: {}", warning);
            mod.getLogger().info("Wayfinder history ready: {} ({} tiles, {} isolated)",
                session.history.path().string(), result->tiles, result->rejected);
            rendering.retryImage();
        } else ui.state.contextLoading = false;
    }
    if (auto result = session.history.finish(force)) applyHistory(std::move(*result));
}
void Wayfinder::Impl::saveHistory(bool force) {
    collectHistory(force);
    if (session.history.canSave(force))
        session.history.save(session.cache.takeChanges(), session.cache.revision(),
            {settings.autosaveSeconds, settings.showBiomeRegions, settings.showLighting, session.overworldSkyDarken}, force);
    if (force) collectHistory(true);
}
void Wayfinder::Impl::applyHistory(MapHistory::Result result) {
    auto& menu = ui.state;
    if (result.biomePoint && session.cursorBiome.point == result.biomePoint && result.error.empty()) {
        auto& cursor = session.cursorBiome;
        cursor.historyRead = true;
        if (cursor.identifier.empty()) cursor.identifier = std::move(result.pointBiome);
    }
    if (result.point && menu.page == MapMenu::Page::Context && menu.contextLoading
        && view.displayedLayer(session.layer) == result.point->layer
        && menu.draft.dimension == result.point->layer.dimension && menu.draft.x == result.point->x
        && menu.draft.z == result.point->z) {
        auto cell = session.cache.get(result.point->layer, result.point->x, result.point->z);
        if (!cell.known()) cell = result.pointCell;
        menu.contextLoading = false;
        if (cell.floor()) menu.draft.y = cell.height;
        else if (!result.error.empty()) menu.fail("Could not read terrain height; try again.");
    }
    for (auto const& warning : result.warnings) mod.getLogger().warn("Wayfinder history: {}", warning);
    if (!result.error.empty()) {
        mod.getLogger().error("Wayfinder history: {}", result.error);
        // Retry the image after the I/O error; do not mark stale pixels current.
        rendering.retryImage();
    } else {
        if (result.view && result.layer == view.displayedLayer(session.layer)
            && result.biomeRegions == settings.showBiomeRegions
            && result.lighting == MapLighting{settings.showLighting, result.layer.dimension == 0 ? session.overworldSkyDarken : 0}) {
            auto visible = view.fullscreen
                ? view.fullView
                : MapView{session.player.x, session.player.z, settings.minimapBlocksPerPixel,
                          settings.minimapPixels, settings.minimapPixels};
            auto expected = detailTextureView(view.fullscreen ? visible : minimapTextureView(visible),
                                              settings.terrainPixelScale);
            // A completed job may belong to an old map mode, scale, or player
            // position. Keep its archive writes, but never roll the minimap back
            // to a texture that cannot cover its current visible rectangle.
            if (result.view->width == expected.width && result.view->height == expected.height
                && result.view->blocksPerPixel == expected.blocksPerPixel
                && (view.fullscreen || textureCoversView(*result.view, visible, settings.terrainPixelScale))) {
                rendering.acceptImage(*result.view, result.layer, result.revision,
                    std::move(result.pixels), std::move(result.biomes), result.biomeRegions, result.lighting);
                rendering.presentView(visible, view.fullscreen);
            }
        }
    }
}
} // namespace wayfinder
