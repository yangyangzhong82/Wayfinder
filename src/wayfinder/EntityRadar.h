#pragma once
#include "wayfinder/Locale.h"
#include "wayfinder/EntityPortraits.h"
#include "wayfinder/MapUi.h"
#include "wayfinder/Settings.h"
#include <span>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace wayfinder {
inline std::string entityTypeId(std::string_view input) {
    auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) throw std::runtime_error("Invalid entity type identifier");
    input = input.substr(first, input.find_last_not_of(" \t\r\n") - first + 1);
    if (input.size() > 128) throw std::runtime_error("Invalid entity type identifier");
    std::string result(input);
    for (char& ch : result) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    if (result.find(':') == std::string::npos) result = "minecraft:" + result;
    auto colon = result.find(':');
    if (colon == 0 || colon + 1 == result.size() || result.find(':', colon + 1) != std::string::npos
        || result.size() > 128) throw std::runtime_error("Invalid entity type identifier");
    for (std::size_t i = 0; i < result.size(); ++i) {
        char ch = result[i];
        if (i == colon || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
            || ch == '_' || ch == '-' || ch == '.' || (ch == '/' && i > colon)) continue;
        throw std::runtime_error("Invalid entity type identifier");
    }
    return result;
}
inline bool selectedEntityType(Settings const& settings, std::string_view type) {
    return std::find(settings.entityTypes.begin(), settings.entityTypes.end(), type) != settings.entityTypes.end();
}
inline bool nonLivingEntityType(std::string_view type) {
    // Also hide obsolete non-living selections from configurations made before
    // the radar became mobs-only. Runtime classification is authoritative for add-ons.
    constexpr std::string_view types[]{
        "minecraft:item", "minecraft:xp_orb", "minecraft:armor_stand", "minecraft:tripod_camera",
        "minecraft:boat", "minecraft:chest_boat", "minecraft:minecart", "minecraft:chest_minecart",
        "minecraft:hopper_minecart", "minecraft:tnt_minecart", "minecraft:command_block_minecart",
        "minecraft:arrow", "minecraft:thrown_trident", "minecraft:snowball", "minecraft:egg",
        "minecraft:ender_pearl", "minecraft:eye_of_ender_signal", "minecraft:xp_bottle",
        "minecraft:splash_potion", "minecraft:lingering_potion", "minecraft:fishing_hook",
        "minecraft:fireball", "minecraft:small_fireball", "minecraft:dragon_fireball",
        "minecraft:wither_skull", "minecraft:wither_skull_dangerous", "minecraft:shulker_bullet",
        "minecraft:llama_spit", "minecraft:evocation_fang", "minecraft:fireworks_rocket",
        "minecraft:tnt", "minecraft:falling_block", "minecraft:moving_block", "minecraft:ender_crystal",
        "minecraft:painting", "minecraft:leash_knot", "minecraft:lightning_bolt", "minecraft:area_effect_cloud"
    };
    return std::find(std::begin(types), std::end(types), type) != std::end(types);
}
inline bool livingRadarEntity(bool player, bool mob, bool alive, std::string_view type) {
    // Armor stands and tripod cameras can be Mob actors but are not creatures.
    return alive && (player || mob) && !nonLivingEntityType(type);
}
inline bool allowedEntityType(Settings const& settings, std::string_view type) {
    return !nonLivingEntityType(type) && (!settings.entityFilterEnabled || selectedEntityType(settings, type));
}
inline std::vector<std::string> entityTypeChoices(std::span<std::string const> observed, Settings const& settings) {
    std::vector<std::string> result{
        "minecraft:player", "minecraft:zombie", "minecraft:skeleton", "minecraft:creeper",
        "minecraft:spider", "minecraft:cave_spider", "minecraft:enderman", "minecraft:witch",
        "minecraft:slime", "minecraft:drowned", "minecraft:husk", "minecraft:stray",
        "minecraft:phantom", "minecraft:warden", "minecraft:pillager", "minecraft:vindicator",
        "minecraft:evocation_illager", "minecraft:ravager", "minecraft:blaze", "minecraft:ghast",
        "minecraft:magma_cube", "minecraft:piglin", "minecraft:piglin_brute", "minecraft:zombie_pigman",
        "minecraft:hoglin", "minecraft:zoglin", "minecraft:shulker", "minecraft:ender_dragon",
        "minecraft:wither", "minecraft:wither_skeleton", "minecraft:cow", "minecraft:sheep",
        "minecraft:pig", "minecraft:chicken", "minecraft:wolf", "minecraft:cat",
        "minecraft:horse", "minecraft:donkey", "minecraft:rabbit", "minecraft:fox",
        "minecraft:bee", "minecraft:bat", "minecraft:goat", "minecraft:frog",
        "minecraft:axolotl", "minecraft:allay", "minecraft:villager", "minecraft:villager_v2",
        "minecraft:wandering_trader", "minecraft:iron_golem", "minecraft:snow_golem",
        "minecraft:squid", "minecraft:glow_squid", "minecraft:dolphin", "minecraft:turtle",
        "minecraft:cod", "minecraft:salmon", "minecraft:tropicalfish", "minecraft:pufferfish"
    };
    result.insert(result.end(), observed.begin(), observed.end());
    result.insert(result.end(), settings.entityTypes.begin(), settings.entityTypes.end());
    std::erase_if(result, [](auto const& type) { return nonLivingEntityType(type); });
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    auto player = std::find(result.begin(), result.end(), "minecraft:player");
    if (player != result.end()) std::rotate(result.begin(), player, player + 1);
    return result;
}
inline std::string entityTypeLabel(std::string const& type, Locale const& locale) {
    auto key = "entity." + type;
    auto label = locale.tr(key);
    return label == key ? type : label;
}
inline bool entityTypeMatches(std::string const& type, std::string const& query, Locale const& locale) {
    auto lower = [](std::string text) {
        for (char& ch : text) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        return text;
    };
    auto needle = lower(query);
    return lower(type).find(needle) != std::string::npos
        || lower(entityTypeLabel(type, locale)).find(needle) != std::string::npos;
}
struct EntityMarker {
    int dimension{};
    double x{}, y{}, z{};
    std::string type, name;
    bool player{};
};
inline bool entityPriority(EntityMarker const& a, EntityMarker const& b, double x, double y, double z) {
    if (a.player != b.player) return a.player;
    auto distance = [&](auto const& e) {
        return (e.x - x) * (e.x - x) + (e.y - y) * (e.y - y) + (e.z - z) * (e.z - z);
    };
    return distance(a) < distance(b);
}
inline void keepNearbyEntity(std::vector<EntityMarker>& heap, EntityMarker entity, double x, double y, double z) {
    auto better = [&](auto const& a, auto const& b) { return entityPriority(a, b, x, y, z); };
    if (heap.size() < 512) {
        heap.push_back(std::move(entity));
        std::push_heap(heap.begin(), heap.end(), better);
    } else if (better(entity, heap.front())) {
        std::pop_heap(heap.begin(), heap.end(), better);
        heap.back() = std::move(entity);
        std::push_heap(heap.begin(), heap.end(), better);
    }
}
inline bool nearbyEntity(EntityMarker const& entity, MapLayer layer, double x, double y, double z, int radius) {
    if (entity.dimension != layer.dimension || !std::isfinite(entity.x) || !std::isfinite(entity.y)
        || !std::isfinite(entity.z)) return false;
    double dx = entity.x - x, dy = entity.y - y, dz = entity.z - z;
    return dx * dx + dy * dy + dz * dz <= double(radius) * radius
        && (!layer.underground() || std::abs(dy) <= 16);
}
struct PlacedEntity {
    float x{}, y{};
    bool player{};
    std::string name;
    MapRect label;
    float textScale{0.6f};
    std::string type;
    float iconScale{1.0f}, opacity{1.0f};
    bool below{};
    float anchorX{}, anchorY{}; // Actual projected position; x/y may move slightly to avoid overlap.
    int count{1};
};
inline float entityIconSize(PlacedEntity const& marker, Settings const& settings) {
    return (settings.entityPortraits && entityPortrait(marker.type) ? entityPortraitSize + 2 : 6) * marker.iconScale;
}
inline MapRect entityCountBounds(PlacedEntity const& marker, Settings const& settings) {
    if (marker.count < 2) return {};
    float size = entityIconSize(marker, settings);
    return {marker.x + size / 2 - 3, marker.y + size / 2 - 3,
        float(std::to_string(marker.count).size()) * 4 + 4, 8};
}
inline MapRect entityVisualBounds(PlacedEntity const& marker, Settings const& settings) {
    float size = entityIconSize(marker, settings);
    MapRect box{marker.x - size / 2, marker.y - size / 2, size, size};
    auto badge = entityCountBounds(marker, settings);
    if (marker.count > 1) {
        box.width = std::max(box.width, badge.x + badge.width - box.x);
        box.height = std::max(box.height, badge.y + badge.height - box.y);
    }
    return box;
}
inline std::vector<PlacedEntity> layoutEntities(
    std::span<EntityMarker const> entities, MapView const& view, MapRect area, MapLayer layer,
    double playerX, double playerY, double playerZ, Settings const& settings, Locale const& locale, bool fullscreen = false
) {
    std::vector<PlacedEntity> result;
    if (!settings.showEntities || area.width < 8 || area.height < 8 || view.width <= 0 || view.height <= 0
        || view.blocksPerPixel <= 0) return result;
    auto origin = view.worldAt(0, 0);
    // World-anchored screen-sized cells stay stable while panning. Never combine
    // different species, height groups or players into one misleading portrait.
    using ClusterKey = std::tuple<double, double, std::string, bool>;
    std::map<ClusterKey, std::size_t> clusters;
    float scale = fullscreen ? 1.0f : settings.minimapEntityScale;
    float cellPixels = std::max(8.0f, (settings.entityPortraits ? entityPortraitSize + 2 : 6) * scale + 2);
    double cellX = cellPixels * view.width * view.blocksPerPixel / area.width;
    double cellZ = cellPixels * view.height * view.blocksPerPixel / area.height;
    for (auto const& entity : entities) {
        if (!allowedEntityType(settings, entity.type)
            || !nearbyEntity(entity, layer, playerX, playerY, playerZ, settings.entityRadius)) continue;
        float x = area.x + float((entity.x - origin[0]) / (view.width * view.blocksPerPixel)) * area.width;
        float y = area.y + float((entity.z - origin[1]) / (view.height * view.blocksPerPixel)) * area.height;
        // Off-screen entities are not pinned to the edge like navigation targets.
        if (!area.contains(x, y)) continue;
        bool below = playerY - entity.y >= settings.entityDepthThreshold;
        if (!entity.player) {
            auto [cluster, inserted] = clusters.try_emplace(
                ClusterKey{std::floor(entity.x / cellX), std::floor(entity.z / cellZ), entity.type, below}, result.size());
            if (!inserted) {
                auto& marker = result[cluster->second];
                ++marker.count;
                marker.x += (x - marker.x) / marker.count;
                marker.y += (y - marker.y) / marker.count;
                marker.anchorX = marker.x; marker.anchorY = marker.y;
                continue;
            }
        }
        result.push_back({x, y, entity.player, entity.player
            ? (entity.name.empty() ? entityTypeLabel("minecraft:player", locale) : entity.name) : "", {}, 0.6f, entity.type});
        auto& marker = result.back();
        marker.iconScale = scale;
        marker.below = below;
        marker.opacity = marker.below ? settings.undergroundEntityOpacity : 1.0f;
        marker.anchorX = x; marker.anchorY = y;
    }
    std::vector<MapRect> occupied;
    float selfX = area.x + float((playerX - origin[0]) / (view.width * view.blocksPerPixel)) * area.width;
    float selfY = area.y + float((playerZ - origin[1]) / (view.height * view.blocksPerPixel)) * area.height;
    if (area.contains(selfX, selfY)) occupied.push_back({selfX - 4, selfY - 4, 8, 8});
    std::vector<std::size_t> order(result.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](auto a, auto b) {
        auto const& lhs = result[a]; auto const& rhs = result[b];
        auto priority = [&](auto const& m) {
            return std::tuple{!m.player, m.below, m.count,
                (m.anchorX-selfX)*(m.anchorX-selfX) + (m.anchorY-selfY)*(m.anchorY-selfY),
                std::string_view(m.type), m.anchorX, m.anchorY, std::string_view(m.name)};
        };
        return priority(lhs) < priority(rhs);
    });
    std::vector<std::array<int, 2>> offsets;
    for (int dy = -3; dy <= 3; ++dy) for (int dx = -3; dx <= 3; ++dx) offsets.push_back({dx, dy});
    std::sort(offsets.begin(), offsets.end(), [](auto a, auto b) {
        return std::tuple{a[0]*a[0]+a[1]*a[1], a[1], a[0]} < std::tuple{b[0]*b[0]+b[1]*b[1], b[1], b[0]};
    });
    for (auto index : order) {
        auto& marker = result[index];
        auto original = entityVisualBounds(marker, settings);
        float paddingX = marker.x - original.x, paddingY = marker.y - original.y;
        auto fits = [&](MapRect const& box) {
            return box.x >= area.x && box.y >= area.y && box.x + box.width <= area.x + area.width
                && box.y + box.height <= area.y + area.height
                && std::none_of(occupied.begin(), occupied.end(), [&](auto const& other) { return box.intersects(other); });
        };
        if (!fits(original)) {
            float step = std::max(4.0f, entityIconSize(marker, settings) + 2);
            // Bounded fan-out keeps icons near the real position. Prefer the
            // smallest displacement; unchanged input produces unchanged placement.
            for (auto [dx, dy] : offsets) {
                float x = marker.anchorX + dx * step, y = marker.anchorY + dy * step;
                if (original.width <= area.width)
                    x = std::clamp(x, area.x + paddingX, area.x + area.width - original.width + paddingX);
                if (original.height <= area.height)
                    y = std::clamp(y, area.y + paddingY, area.y + area.height - original.height + paddingY);
                MapRect box{x-paddingX, y-paddingY, original.width, original.height};
                if (!fits(box)) continue;
                marker.x = x; marker.y = y; break;
            }
        }
        occupied.push_back(entityVisualBounds(marker, settings));
    }
    for (auto index : order) {
        auto& marker = result[index];
        if (!marker.player) continue;
        float width = 6;
        for (unsigned char ch : marker.name) if ((ch & 0xc0) != 0x80) width += ch < 0x80 ? 3.5f : 6.0f;
        float naturalWidth = width;
        width = std::min(width, area.width - 2);
        marker.textScale *= width / naturalWidth;
        float height = std::min(11.0f, area.height - 2);
        auto fit = [&](float x, float y) {
            return MapRect{std::clamp(x, area.x + 1, area.x + area.width - width - 1),
                std::clamp(y, area.y + 1, area.y + area.height - height - 1), width, height};
        };
        float gap = 3 * marker.iconScale + 2;
        marker.label = fit(marker.x + gap, marker.y - 5); // Keep names even if every candidate is occupied.
        bool placed = false;
        for (int ring = 0; ring <= 3 && !placed; ++ring) {
            float offset = gap + ring * (height + 2);
            std::array<MapRect, 4> choices{{fit(marker.x + offset, marker.y - 5), fit(marker.x - width - offset, marker.y - 5),
                fit(marker.x - width / 2, marker.y - height - offset), fit(marker.x - width / 2, marker.y + offset)}};
            for (auto choice : choices) {
                if (std::none_of(occupied.begin(), occupied.end(), [&](auto box) { return choice.intersects(box); })) {
                    marker.label = choice; placed = true; break;
                }
            }
        }
        occupied.push_back(marker.label);
    }
    return result;
}
} // namespace wayfinder
