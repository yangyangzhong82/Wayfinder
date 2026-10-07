#include "wayfinder/WayfinderInternal.h"

namespace wayfinder {
void Wayfinder::Impl::invalidateDisplayedMap() {
    session.history.clearRequests();
    session.cursorBiome = {};
    session.lastCursorBiome = {};
    rendering.invalidate();
    input.stopGesture();
    // Pointer queries and a selected teleport destination belong to the old view.
    if (ui.state.page == MapMenu::Page::Context) ui.state.open(MapMenu::Page::Map);
}

void Wayfinder::Impl::followPlayer() {
    view.follow(session.player.x, session.player.z);
    invalidateDisplayedMap();
    ui.state.open(MapMenu::Page::Map);
}

void Wayfinder::Impl::fitKnownArea() {
    auto layer = view.displayedLayer(session.layer);
    auto const& bounds = session.history.layerBounds();
    auto found = bounds.find(layer);
    view.fit(session.cache.bounds(layer), found == bounds.end()
        ? std::nullopt : std::optional<MapBounds>(found->second));
}

void Wayfinder::Impl::selectDisplayedLayer(MapLayer layer) {
    view.selectLayer(layer);
    fitKnownArea();
    invalidateDisplayedMap();
    ui.state.open(MapMenu::Page::Map);
}

void Wayfinder::Impl::changeDimension(int dimension) {
    if (session.player.dimension == dimension) return;
    closeMap(true);
    session.player.dimension = dimension;
    session.layer = MapLayer{dimension};
    session.entities.clear();
    session.lastEntities = {};
    session.player.biome.clear();
    session.player.lastBiome = {};
    session.sampler.reset();
    invalidateDisplayedMap();
}

void Wayfinder::Impl::changeLiveLayer(MapLayer layer) {
    if (session.layer == layer) return;
    auto previousDisplay = view.displayedLayer(session.layer);
    session.layer = layer;
    session.entities.clear();
    session.lastEntities = {};
    session.sampler.reset();
    // Browsing a locked historical layer must survive changes to the live layer.
    if (previousDisplay != view.displayedLayer(session.layer)) invalidateDisplayedMap();
}
} // namespace wayfinder
