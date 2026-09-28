#include "wayfinder/WayfinderInternal.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockSource.h"
#include <unordered_set>

namespace wayfinder {
void Wayfinder::Impl::Session::sampleEntities(Impl& app) {
    if (!app.settings.showEntities && app.ui.state.page != MapMenu::Page::Entities) {
        entities.clear();
        lastEntities = {};
        app.ui.state.loadedEntities = app.ui.state.nearbyEntities = app.ui.state.visibleEntities = 0;
        return;
    }
    if (Clock::now() - lastEntities < std::chrono::milliseconds(250)) return;
    lastEntities = Clock::now();
    entities.clear();
    app.ui.state.loadedEntities = app.ui.state.nearbyEntities = app.ui.state.visibleEntities = 0;
    auto source = client ? client->getRegion() : nullptr;
    auto self = client ? client->getLocalPlayer() : nullptr;
    auto level = client ? client->getLevel() : nullptr;
    if (!source || !self || !level || static_cast<int>(source->getDimensionId()) != player.dimension) return;
    // Read the client level's live actors directly. BlockSource's spatial/type
    // query and isInWorld gate are not required for a client radar snapshot.
    auto nearby = level->getRuntimeActorList();
    // Players can be owned by the level's user list instead of its actor list.
    level->forEachPlayer([&](::Player& other) { nearby.push_back(&other); return true; });
    std::unordered_set<::Actor*> visited;
    entities.reserve(512);
    for (auto actor : nearby) {
        if (!actor || actor == self || !visited.insert(actor).second || actor->mRemoved) continue;
        bool isPlayer = actor->isPlayer();
        auto actorType = actor->getEntityTypeId();
        // Built-in ActorType values also carry the Mob bit. Use it alongside
        // categories, so a missing client category cannot hide a vanilla mob.
        bool isMob = (actor->hasCategory(ActorCategory::Mob)
            || (static_cast<unsigned>(actorType) & static_cast<unsigned>(ActorType::Mob)) != 0)
            && actorType != ActorType::ArmorStand && actorType != ActorType::TripodCamera;
        if (!isPlayer && !isMob) continue;
        std::string type;
        try { type = entityTypeId(isPlayer ? "minecraft:player" : actor->getTypeName()); }
        catch (std::exception const&) { continue; }
        // Mob categories include add-on creatures without relying on portrait
        // coverage. Reject non-living actors before counts, catalogs or heap slots.
        if (!livingRadarEntity(isPlayer, isMob, actor->isAlive(), type)) continue;
        ++app.ui.state.loadedEntities;
        auto const& pos = actor->getPosition();
        EntityMarker marker{static_cast<int>(actor->getDimensionId()), pos.x, pos.y, pos.z, std::move(type), {}, isPlayer};
        if (!nearbyEntity(marker, MapLayer{player.dimension}, player.x, player.y, player.z, app.settings.entityRadius)) continue;
        ++app.ui.state.nearbyEntities;
        auto& observed = app.ui.state.observedEntityTypes;
        if (observed.size() < 256 && std::find(observed.begin(), observed.end(), marker.type) == observed.end())
            observed.push_back(marker.type);
        if (!app.settings.showEntities || !allowedEntityType(app.settings, marker.type)
            || !nearbyEntity(marker, layer, player.x, player.y, player.z, app.settings.entityRadius)) continue;
        if (isPlayer) {
            // isPlayer validates the derived type before the static cast; RTTI is not used.
            auto name = static_cast<::Player const*>(actor)->getRealName();
            if (name.empty()) name = actor->getNameTag();
            try { marker.name = cleanName(name); } catch (std::exception const&) { marker.name.clear(); }
        }
        keepNearbyEntity(entities, std::move(marker), player.x, player.y, player.z);
    }
    app.ui.state.visibleEntities = static_cast<int>(entities.size());
    std::sort(entities.begin(), entities.end(), [&](auto const& a, auto const& b) {
        return entityPriority(a, b, player.x, player.y, player.z);
    });
}
} // namespace wayfinder
