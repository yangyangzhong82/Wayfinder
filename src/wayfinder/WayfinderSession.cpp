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

void Wayfinder::Impl::persistNavigation() {
    if (!session.navigationDirty || session.identity.empty()) return;
    session.lastNavigationAttempt = Clock::now();
    try {
        writeNavigation(session.navigationPath, session.identity, session.navigation);
        session.navigationDirty = false;
    } catch (std::exception const& ex) {
        ui.state.message = "Waypoint save failed; will retry";
        mod.getLogger().error("Wayfinder waypoints: {}", ex.what());
    }
}

void Wayfinder::Impl::beginSession(IClientInstance& ci) {
    auto levelId = ci.getLevel()->getLevelId();
    bool multiplayer = ci.isPrimaryLevelMultiplayer();
    // Multiplayer is diagnostic only: local worlds can report true as well.
    // Missing identity metadata must not block rendering or map hotkeys.
    auto storageIdentity = worldStorageIdentity(
        settings.cacheProfile, levelId,
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));

    collectHistory(true);
    session.client = &ci;
    session.cache.clear();
    session.sampler.reset();
    rendering.invalidate();
    rendering.resetResources();
    renderRecovery = {};
    session.player.dimension = static_cast<int>(ci.getLocalPlayer()->getDimensionId());
    session.layer = MapLayer{session.player.dimension};
    session.overworldSkyDarken = 0;
    session.entities.clear();
    session.lastEntities = {};
    ui.state.observedEntityTypes.clear();
    ui.state.entitySearch.clear();
    ui.state.loadedEntities = ui.state.nearbyEntities = ui.state.visibleEntities = 0;
    session.identity = std::move(storageIdentity);
    session.history.start(mod.getDataDir() / "maps" / storageName(session.identity),
        session.identity, session.cache.capacity());
    session.navigationPath = mod.getDataDir() / "waypoints" / storageName(session.identity);
    session.navigationPath.replace_extension(".json");
    session.navigation = {};
    session.navigationDirty = false;
    session.deathTracker.reset();
    session.trail = {};
    session.savedTrailRevision = 0;
    session.trailSaveFailed = false;
    session.lastTrailSave = Clock::now();
    session.trailPath = mod.getDataDir() / "trails" / storageName(session.identity);
    session.trailPath.replace_extension(".json");
    try { session.trail = readTrail(session.trailPath, session.identity); }
    catch (std::exception const& ex) {
        mod.getLogger().warn("Wayfinder could not load trail: {}", ex.what());
        session.trailPath += ".recovery-" + std::to_string(unixTime());
    }
    view.lockedLayer.reset();
    ui.state.knownLayers.clear();
    session.player.biome.clear();
    session.player.lastBiome = {};
    session.cursorBiome = {};
    session.lastCursorBiome = {};
    ui.state.open(MapMenu::Page::Map);
    ui.state.query = {};
    try {
        session.navigation = readNavigation(session.navigationPath, session.identity);
    } catch (std::exception const& ex) {
        mod.getLogger().warn("Wayfinder could not load waypoints: {}", ex.what());
        session.navigationPath += ".recovery-" + std::to_string(unixTime());
    }
    mod.getLogger().info("Wayfinder session: multiplayer={}, levelId='{}', identity='{}'",
                            multiplayer, levelId, session.identity);
    mod.getLogger().info("Wayfinder map: {} ({} cached chunks)", session.history.path().string(), session.cache.size());
    mod.getLogger().info("Wayfinder waypoints: {} ({} loaded, identity: {})",
                            session.navigationPath.string(), session.navigation.points.size(), session.identity);
    if (session.identity.starts_with("session:"))
        mod.getLogger().info(
            "No LevelId is available; history and waypoints are isolated per session. "
            "Set a unique cacheProfile for this world to reuse them.");
}

void Wayfinder::Impl::endSession() {
    closeMap(false);
    persistNavigation();
    persistTrail(true);
    session.deathTracker.reset();
    session.history.clearRequests();
    saveHistory(true);
    if (session.history.needsRetry()) saveHistory(true);
    session.history.reset();
    session.client = nullptr;
    session.cursorBiome = {};
    session.lastCursorBiome = {};
    session.identity.clear();
    session.entities.clear();
    session.lastEntities = {};
    ui.state.observedEntityTypes.clear();
    session.cache.clear();
    ui.state.loadedEntities = ui.state.nearbyEntities = ui.state.visibleEntities = 0;
    session.sampler.reset();
    rendering.invalidate();
    input.clearKeys();
    input.stopGesture();
    rendering.resetResources();
    renderRecovery = {};
}

void Wayfinder::Impl::tick(ll::event::ClientLevelTickEvent& event) {
    if (!session.client) {
        auto ci = ll::service::getClientInstance();
        if (!ci || !ci->isPrimaryClient() || !ci->getLocalPlayer() || ci->getLevel() != &event.level()) return;
        beginSession(*ci);
    }
    if (session.client->getLevel() != &event.level()) return;
    auto localPlayer = session.client->getLocalPlayer();
    auto source = session.client->getRegion();
    if (!localPlayer) return;
    bool died = session.deathTracker.observe(localPlayer->isAlive());
    if (!localPlayer->isAlive()) session.trail.stopRetrace();
    if (died && settings.recordDeaths) {
        auto const& deathPos = localPlayer->getPosition();
        if (std::isfinite(deathPos.x) && std::isfinite(deathPos.y) && std::isfinite(deathPos.z) &&
            std::abs(deathPos.x) <= 29999984 && std::abs(deathPos.z) <= 29999984 && deathPos.y >= -32768 &&
            deathPos.y <= 32767) {
            session.navigation.recordDeath(static_cast<int>(localPlayer->getDimensionId()), deathPos.x, deathPos.y, deathPos.z,
                                   unixTime());
            session.navigationDirty = true;
            persistNavigation();
        }
    }
    if (session.navigationDirty && Clock::now() - session.lastNavigationAttempt >= std::chrono::seconds(5)) persistNavigation();
    persistTrail(false);
    if (!source) { session.trail.stopRetrace(); return; }
    // Text editing owns an embedded native control while the map is open.
    if (!foreground() && !ui.textInput.active()) {
        session.trail.stopRetrace();
        closeMap(false);
        input.clearKeys();
        return;
    }
    if (view.fullscreen && !ui.textInput.active() && !hud(*session.client)) closeMap(false);
    auto const& pos = localPlayer->getPosition();
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z) || std::abs(pos.x) > 29999000 ||
        std::abs(pos.z) > 29999000 || pos.y < -32768 || pos.y > 32767)
        return;
    session.player.x = pos.x;
    session.player.y = pos.y;
    session.player.z = pos.z;
    session.player.yaw = localPlayer->getRotation().y;
    ui.state.query.playerX = session.player.x;
    ui.state.query.playerZ = session.player.z;
    ui.state.query.playerDimension = static_cast<int>(localPlayer->getDimensionId());
    int newDimension = static_cast<int>(localPlayer->getDimensionId());
    changeDimension(newDimension);
    // Client region and player dimension may briefly disagree during a portal transition.
    if (static_cast<int>(source->getDimensionId()) != session.player.dimension) { session.trail.stopRetrace(); return; }
    if (session.player.dimension == 0) session.overworldSkyDarken = std::clamp<int>(source->getDimension().mSkyDarken->mValue, 0, 15);
    auto nextLayer = TerrainSampler::selectLayer(session.player.dimension, blockCoordinate(session.player.y), settings, session.layer);
    changeLiveLayer(nextLayer);
    if (localPlayer->isAlive()) session.trail.observe(session.layer, session.player.x, session.player.y, session.player.z, unixTime(), settings.recordTrail);
    if (std::find(ui.state.knownLayers.begin(), ui.state.knownLayers.end(), session.layer) == ui.state.knownLayers.end())
        ui.state.knownLayers.push_back(session.layer);
    sampleEntities();
    if (!settings.showBiome || !view.fullscreen || ui.state.page != MapMenu::Page::Map)
        session.cursorBiome.select({});
    if (session.cursorBiome.point && Clock::now() - session.lastCursorBiome >= std::chrono::milliseconds(100)) {
        session.lastCursorBiome = Clock::now();
        auto const& point = *session.cursorBiome.point;
        auto name = TerrainSampler::biome(*source, point.layer, point.x, point.z, settings);
        if (name.empty()) name = session.cache.biome(point.layer, point.x, point.z);
        if (!name.empty()) session.cursorBiome.identifier = std::move(name);
        else if (!session.cursorBiome.historyRead && session.history.available())
            session.history.requestBiome(point);
    }
    if (settings.showBiome && Clock::now() - session.player.lastBiome >= std::chrono::milliseconds(500)) {
        session.player.lastBiome = Clock::now();
        BlockPos position{blockCoordinate(session.player.x), blockCoordinate(session.player.y), blockCoordinate(session.player.z)};
        auto chunk = source->getChunk(floorDiv(position.x, 16), floorDiv(position.z, 16));
        if (chunk && chunk->mLoadState->load(std::memory_order_acquire) == ChunkState::Loaded)
            session.player.biome = source->getBiome(position).mHash->getString();
        else session.player.biome.clear();
    }
    // Bound pending data when disk I/O stalls; pause discovery instead of
    // discarding unsaved evictions or allowing an unbounded retry queue.
    if (session.history.samplingAllowed() && session.cache.pendingEvictions() < session.cache.capacity())
        session.sampler.tick(*source, session.layer, static_cast<int>(std::floor(session.player.x)),
                     static_cast<int>(std::floor(session.player.z)), session.cache, settings, session.client->getTextureGroup().get());
    collectHistory();
    saveHistory();
}

void Wayfinder::Impl::Session::changed(ll::event::BlockChangedEvent& event) {
    if (!client || client->getRegion() != &event.blockSource()) return;
    sampler.markDirty(static_cast<int>(event.blockSource().getDimensionId()), event.pos().x, event.pos().z);
}

void Wayfinder::Impl::exit(ll::event::ClientExitLevelEvent& event) {
    if (session.client == &event.self()) endSession();
}

} // namespace wayfinder
