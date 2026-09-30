#include "wayfinder/WayfinderInternal.h"
#include "wayfinder/WorldIdentity.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/service/Bedrock.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/biome/Biome.h"
#include "mc/world/level/chunk/LevelChunk.h"

namespace wayfinder {
std::int64_t Wayfinder::Impl::unixTime() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void Wayfinder::Impl::Session::persistNavigation(Impl& app) {
    if (!navigationDirty || identity.empty()) return;
    lastNavigationAttempt = Clock::now();
    try {
        writeNavigation(navigationPath, identity, navigation);
        navigationDirty = false;
    } catch (std::exception const& ex) {
        app.ui.state.message = "Waypoint save failed; will retry";
        app.mod.getLogger().error("Wayfinder waypoints: {}", ex.what());
    }
}

void Wayfinder::Impl::Session::begin(Impl& app, IClientInstance& ci) {
    auto levelId = ci.getLevel()->getLevelId();
    bool multiplayer = ci.isPrimaryLevelMultiplayer();
    // Multiplayer is diagnostic only: local worlds can report true as well.
    // Missing identity metadata must not block rendering or map hotkeys.
    auto storageIdentity = worldStorageIdentity(
        app.settings.cacheProfile, levelId,
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));

    history.finish(app);
    client = &ci;
    cache.clear();
    history.retryChanges.clear();
    history.requestedView.reset();
    history.requestedPoint.reset();
    history.requestedBiome.reset();
    history.bounds.clear();
    history.lastError = {};
    sampler.reset();
    app.rendering.pixels.clear();
    app.rendering.resetTexture = true;
    app.rendering.failed = false;
    player.dimension = static_cast<int>(ci.getLocalPlayer()->getDimensionId());
    layer = MapLayer{player.dimension};
    overworldSkyDarken = 0;
    entities.clear();
    lastEntities = {};
    app.ui.state.observedEntityTypes.clear();
    app.ui.state.entitySearch.clear();
    app.ui.state.loadedEntities = app.ui.state.nearbyEntities = app.ui.state.visibleEntities = 0;
    identity = std::move(storageIdentity);
    history.savePath = app.mod.getDataDir() / "maps" / storageName(identity);
    history.archive.reset();
    history.start(app);
    navigationPath = app.mod.getDataDir() / "waypoints" / storageName(identity);
    navigationPath.replace_extension(".json");
    navigation = {};
    navigationDirty = false;
    deathTracker.reset();
    trail = {};
    savedTrailRevision = 0;
    trailSaveFailed = false;
    lastTrailSave = Clock::now();
    trailPath = app.mod.getDataDir() / "trails" / storageName(identity);
    trailPath.replace_extension(".json");
    try { trail = readTrail(trailPath, identity); }
    catch (std::exception const& ex) {
        app.mod.getLogger().warn("Wayfinder could not load trail: {}", ex.what());
        trailPath += ".recovery-" + std::to_string(unixTime());
    }
    app.view.lockedLayer.reset();
    app.ui.state.knownLayers.clear();
    player.biome.clear();
    player.lastBiome = {};
    cursorBiome = {};
    lastCursorBiome = {};
    app.ui.state.open(MapMenu::Page::Map);
    app.ui.state.query = {};
    try {
        navigation = readNavigation(navigationPath, identity);
    } catch (std::exception const& ex) {
        app.mod.getLogger().warn("Wayfinder could not load waypoints: {}", ex.what());
        navigationPath += ".recovery-" + std::to_string(unixTime());
    }
    history.lastSave = Clock::now();
    app.mod.getLogger().info("Wayfinder session: multiplayer={}, levelId='{}', identity='{}'",
                            multiplayer, levelId, identity);
    app.mod.getLogger().info("Wayfinder map: {} ({} cached chunks)", history.savePath.string(), cache.size());
    app.mod.getLogger().info("Wayfinder waypoints: {} ({} loaded, identity: {})",
                            navigationPath.string(), navigation.points.size(), identity);
    if (identity.starts_with("session:"))
        app.mod.getLogger().info(
            "No LevelId is available; history and waypoints are isolated per session. "
            "Set a unique cacheProfile for this world to reuse them.");
}

void Wayfinder::Impl::Session::end(Impl& app) {
    app.input.closeMap(app, false);
    persistNavigation(app);
    persistTrail(app, true);
    deathTracker.reset();
    history.requestedView.reset();
    history.save(app, true);
    if (!history.retryChanges.empty()) history.save(app, true);
    history.archive.reset();
    history.progress.reset();
    history.retryChanges.clear();
    history.bounds.clear();
    client = nullptr;
    history.requestedBiome.reset();
    cursorBiome = {};
    lastCursorBiome = {};
    identity.clear();
    entities.clear();
    lastEntities = {};
    app.ui.state.observedEntityTypes.clear();
    cache.clear();
    app.ui.state.loadedEntities = app.ui.state.nearbyEntities = app.ui.state.visibleEntities = 0;
    sampler.reset();
    app.rendering.pixels.clear();
    app.input.heldKeys.clear();
    app.input.consumedKeys.clear();
    app.input.consumedMouse.clear();
    app.input.dragging = false;
    app.rendering.resetTexture = true;
}

void Wayfinder::Impl::Session::tick(Impl& app, ll::event::ClientLevelTickEvent& event) {
    if (!client) {
        auto ci = ll::service::getClientInstance();
        if (!ci || !ci->isPrimaryClient() || !ci->getLocalPlayer() || ci->getLevel() != &event.level()) return;
        begin(app, *ci);
    }
    if (client->getLevel() != &event.level()) return;
    auto localPlayer = client->getLocalPlayer();
    auto source = client->getRegion();
    if (!localPlayer) return;
    bool died = deathTracker.observe(localPlayer->isAlive());
    if (!localPlayer->isAlive()) trail.stopRetrace();
    if (died && app.settings.recordDeaths) {
        auto const& deathPos = localPlayer->getPosition();
        if (std::isfinite(deathPos.x) && std::isfinite(deathPos.y) && std::isfinite(deathPos.z) &&
            std::abs(deathPos.x) <= 29999984 && std::abs(deathPos.z) <= 29999984 && deathPos.y >= -32768 &&
            deathPos.y <= 32767) {
            navigation.recordDeath(static_cast<int>(localPlayer->getDimensionId()), deathPos.x, deathPos.y, deathPos.z,
                                   unixTime());
            navigationDirty = true;
            persistNavigation(app);
        }
    }
    if (navigationDirty && Clock::now() - lastNavigationAttempt >= std::chrono::seconds(5)) persistNavigation(app);
    persistTrail(app, false);
    if (!source) { trail.stopRetrace(); return; }
    if (!foreground()) {
        trail.stopRetrace();
        app.input.closeMap(app, false);
        app.input.heldKeys.clear();
        app.input.consumedKeys.clear();
        app.input.consumedMouse.clear();
        return;
    }
    if (app.view.fullscreen && !hud(*client)) app.input.closeMap(app, false);
    auto const& pos = localPlayer->getPosition();
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z) || std::abs(pos.x) > 29999000 ||
        std::abs(pos.z) > 29999000 || pos.y < -32768 || pos.y > 32767)
        return;
    player.x = pos.x;
    player.y = pos.y;
    player.z = pos.z;
    player.yaw = localPlayer->getRotation().y;
    app.ui.state.query.playerX = player.x;
    app.ui.state.query.playerZ = player.z;
    app.ui.state.query.playerDimension = static_cast<int>(localPlayer->getDimensionId());
    int newDimension = static_cast<int>(localPlayer->getDimensionId());
    if (player.dimension != newDimension) {
        app.input.closeMap(app, true);
        player.dimension = newDimension;
        entities.clear();
        lastEntities = {};
        layer = MapLayer{newDimension};
        history.requestedView.reset();
        history.requestedPoint.reset();
        history.requestedBiome.reset();
        player.biome.clear();
        player.lastBiome = {};
        cursorBiome = {};
        lastCursorBiome = {};
        sampler.reset();
        app.rendering.pixels.clear();
    }
    // Client region and player dimension may briefly disagree during a portal transition.
    if (static_cast<int>(source->getDimensionId()) != player.dimension) { trail.stopRetrace(); return; }
    if (player.dimension == 0) overworldSkyDarken = std::clamp<int>(source->getDimension().mSkyDarken->mValue, 0, 15);
    auto nextLayer = TerrainSampler::selectLayer(player.dimension, blockCoordinate(player.y), app.settings, layer);
    if (nextLayer != layer) {
        auto oldDisplay = app.view.displayedLayer(layer);
        layer = nextLayer;
        entities.clear();
        lastEntities = {};
        sampler.reset();
        if (oldDisplay != app.view.displayedLayer(layer)) {
            history.requestedView.reset();
            history.requestedPoint.reset();
            app.rendering.pixels.clear();
            app.rendering.lastRaster = {};
            // A selected teleport destination belongs to its old floor.
            if (app.ui.state.page == MapMenu::Page::Context) app.ui.state.open(MapMenu::Page::Map);
        }
    }
    if (localPlayer->isAlive()) trail.observe(layer, player.x, player.y, player.z, unixTime(), app.settings.recordTrail);
    if (std::find(app.ui.state.knownLayers.begin(), app.ui.state.knownLayers.end(), layer) == app.ui.state.knownLayers.end())
        app.ui.state.knownLayers.push_back(layer);
    sampleEntities(app);
    if (!app.settings.showBiome || !app.view.fullscreen || app.ui.state.page != MapMenu::Page::Map)
        cursorBiome.select({});
    if (cursorBiome.point && Clock::now() - lastCursorBiome >= std::chrono::milliseconds(100)) {
        lastCursorBiome = Clock::now();
        auto const& point = *cursorBiome.point;
        auto name = TerrainSampler::biome(*source, point.layer, point.x, point.z, app.settings);
        if (name.empty()) name = cache.biome(point.layer, point.x, point.z);
        if (!name.empty()) cursorBiome.identifier = std::move(name);
        else if (!cursorBiome.historyRead && (history.archive || history.openTask.valid()))
            history.requestedBiome = point;
    }
    if (app.settings.showBiome && Clock::now() - player.lastBiome >= std::chrono::milliseconds(500)) {
        player.lastBiome = Clock::now();
        BlockPos position{blockCoordinate(player.x), blockCoordinate(player.y), blockCoordinate(player.z)};
        auto chunk = source->getChunk(floorDiv(position.x, 16), floorDiv(position.z, 16));
        if (chunk && chunk->mLoadState->load(std::memory_order_acquire) == ChunkState::Loaded)
            player.biome = source->getBiome(position).mHash->getString();
        else player.biome.clear();
    }
    // Bound pending data when disk I/O stalls; pause discovery instead of
    // discarding unsaved evictions or allowing an unbounded retry queue.
    if (history.retryChanges.empty() && cache.pendingEvictions() < cache.capacity() &&
        Clock::now() - history.lastError >= std::chrono::seconds(5))
        sampler.tick(*source, layer, static_cast<int>(std::floor(player.x)),
                     static_cast<int>(std::floor(player.z)), cache, app.settings, client->getTextureGroup().get());
    if (history.ready()) history.finish(app);
    history.save(app, false);
}

void Wayfinder::Impl::Session::changed(ll::event::BlockChangedEvent& event) {
    if (!client || client->getRegion() != &event.blockSource()) return;
    sampler.markDirty(static_cast<int>(event.blockSource().getDimensionId()), event.pos().x, event.pos().z);
}

void Wayfinder::Impl::Session::exit(Impl& app, ll::event::ClientExitLevelEvent& event) {
    if (client == &event.self()) end(app);
}

} // namespace wayfinder
