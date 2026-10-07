#include "wayfinder/WayfinderInternal.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockSource.h"
#include <unordered_set>

namespace wayfinder {
void Wayfinder::Impl::sampleEntities() {
    if (!settings.showEntities && ui.state.page != MapMenu::Page::Entities) {
        session.entities.clear();
        session.lastEntities = {};
        ui.state.loadedEntities = ui.state.nearbyEntities = ui.state.visibleEntities = 0;
        return;
    }
    if (Clock::now() - session.lastEntities < std::chrono::milliseconds(250)) return;
    session.lastEntities = Clock::now();
    session.entities.clear();
    ui.state.loadedEntities = ui.state.nearbyEntities = ui.state.visibleEntities = 0;
    auto source = session.client ? session.client->getRegion() : nullptr;
    auto self = session.client ? session.client->getLocalPlayer() : nullptr;
    auto level = session.client ? session.client->getLevel() : nullptr;
    if (!source || !self || !level || static_cast<int>(source->getDimensionId()) != session.player.dimension) return;
    // Read the client level's live actors directly. BlockSource's spatial/type
    // query and isInWorld gate are not required for a client radar snapshot.
    auto nearby = level->getRuntimeActorList();
    // Players can be owned by the level's user list instead of its actor list.
    level->forEachPlayer([&](::Player& other) { nearby.push_back(&other); return true; });
    std::unordered_set<::Actor*> visited;
    session.entities.reserve(512);
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
        ++ui.state.loadedEntities;
        auto const& pos = actor->getPosition();
        EntityMarker marker{static_cast<int>(actor->getDimensionId()), pos.x, pos.y, pos.z, std::move(type), {}, isPlayer};
        if (!nearbyEntity(marker, MapLayer{session.player.dimension}, session.player.x, session.player.y, session.player.z, settings.entityRadius)) continue;
        ++ui.state.nearbyEntities;
        auto& observed = ui.state.observedEntityTypes;
        if (observed.size() < 256 && std::find(observed.begin(), observed.end(), marker.type) == observed.end())
            observed.push_back(marker.type);
        if (!settings.showEntities || !allowedEntityType(settings, marker.type)
            || !nearbyEntity(marker, session.layer, session.player.x, session.player.y, session.player.z, settings.entityRadius)) continue;
        if (isPlayer) {
            // isPlayer validates the derived type before the static cast; RTTI is not used.
            auto name = static_cast<::Player const*>(actor)->getRealName();
            if (name.empty()) name = actor->getNameTag();
            try { marker.name = cleanName(name); } catch (std::exception const&) { marker.name.clear(); }
        } else {
            marker.disposition = entityDisposition(marker.type, actor->getStatusFlag(ActorFlags::Tamed),
                actor->getStatusFlag(ActorFlags::Angry), actor->hasCategory(ActorCategory::Monster),
                actor->hasCategory(ActorCategory::Animal) || actor->hasCategory(ActorCategory::Villager)
                    || actor->hasCategory(ActorCategory::Ambient));
        }
        keepNearbyEntity(session.entities, std::move(marker), session.player.x, session.player.y, session.player.z);
    }
    ui.state.visibleEntities = static_cast<int>(session.entities.size());
    std::sort(session.entities.begin(), session.entities.end(), [&](auto const& a, auto const& b) {
        return entityPriority(a, b, session.player.x, session.player.y, session.player.z);
    });
}
} // namespace wayfinder
