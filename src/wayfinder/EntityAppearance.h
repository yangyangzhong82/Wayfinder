#pragma once
#include "wayfinder/MapCore.h"
#include <string_view>

namespace wayfinder {
enum class EntityDisposition { Tamed, Friendly, Neutral, Hostile, Boss };

inline EntityDisposition entityDisposition(
    std::string_view type, bool tamed = false, bool angry = false, bool monster = false, bool friendly = false
) {
    if (type == "minecraft:ender_dragon" || type == "minecraft:wither") return EntityDisposition::Boss;
    if (tamed) return EntityDisposition::Tamed;
    if (angry) return EntityDisposition::Hostile;
    // These species can retaliate or attack conditionally, even when categorized as monsters.
    constexpr std::string_view neutral[]{
        "minecraft:bee", "minecraft:wolf", "minecraft:enderman", "minecraft:piglin",
        "minecraft:zombie_pigman", "minecraft:zombified_piglin", "minecraft:spider", "minecraft:cave_spider",
        "minecraft:iron_golem", "minecraft:polar_bear", "minecraft:panda", "minecraft:goat",
        "minecraft:llama", "minecraft:trader_llama", "minecraft:dolphin", "minecraft:pufferfish"
    };
    if (std::find(std::begin(neutral), std::end(neutral), type) != std::end(neutral)) return EntityDisposition::Neutral;
    constexpr std::string_view hostile[]{
        "minecraft:zombie", "minecraft:husk", "minecraft:drowned", "minecraft:skeleton", "minecraft:stray",
        "minecraft:wither_skeleton", "minecraft:bogged", "minecraft:creeper", "minecraft:witch", "minecraft:slime",
        "minecraft:magma_cube", "minecraft:phantom", "minecraft:warden", "minecraft:pillager",
        "minecraft:vindicator", "minecraft:evocation_illager", "minecraft:evoker", "minecraft:vex",
        "minecraft:ravager", "minecraft:blaze", "minecraft:ghast", "minecraft:piglin_brute", "minecraft:hoglin",
        "minecraft:zoglin", "minecraft:shulker", "minecraft:silverfish", "minecraft:endermite",
        "minecraft:guardian", "minecraft:elder_guardian", "minecraft:breeze", "minecraft:creaking"
    };
    if (monster || std::find(std::begin(hostile), std::end(hostile), type) != std::end(hostile))
        return EntityDisposition::Hostile;
    constexpr std::string_view passive[]{
        "minecraft:cow", "minecraft:mooshroom", "minecraft:sheep", "minecraft:pig", "minecraft:chicken",
        "minecraft:cat", "minecraft:ocelot", "minecraft:horse", "minecraft:donkey", "minecraft:mule",
        "minecraft:skeleton_horse", "minecraft:zombie_horse", "minecraft:rabbit", "minecraft:fox",
        "minecraft:bat", "minecraft:frog", "minecraft:tadpole", "minecraft:axolotl", "minecraft:allay",
        "minecraft:villager", "minecraft:villager_v2", "minecraft:wandering_trader", "minecraft:snow_golem",
        "minecraft:squid", "minecraft:glow_squid", "minecraft:turtle", "minecraft:cod",
        "minecraft:salmon", "minecraft:tropicalfish", "minecraft:parrot", "minecraft:strider",
        "minecraft:camel", "minecraft:sniffer", "minecraft:armadillo", "minecraft:happy_ghast"
    };
    if (friendly || std::find(std::begin(passive), std::end(passive), type) != std::end(passive))
        return EntityDisposition::Friendly;
    return EntityDisposition::Neutral; // Unknown add-ons must not be assumed harmless.
}

// One small texture for all relationship colors, including the existing cyan player marker.
struct EntityCircleAtlas {
    static constexpr int tileSize = 16, columns = 6, width = tileSize * columns, height = tileSize;
    static constexpr std::array<std::uint32_t, columns> colors{
        rgba(90, 155, 255), rgba(95, 225, 115), rgba(255, 215, 70),
        rgba(255, 85, 85), rgba(200, 105, 255), rgba(70, 225, 255)
    };
    static int index(EntityDisposition disposition, bool player) { return player ? 5 : static_cast<int>(disposition); }
    static std::vector<std::uint32_t> pixels() {
        std::vector<std::uint32_t> result(width * height);
        for (int y = 0; y < height; ++y) for (int x = 0; x < tileSize; ++x) {
            float dx = x + 0.5f - tileSize / 2, dy = y + 0.5f - tileSize / 2;
            float radius = std::sqrt(dx * dx + dy * dy);
            // A transparent outer texel isolates adjacent colors during linear filtering.
            float coverage = std::clamp(7.5f - radius, 0.0f, 1.0f) * std::clamp(radius - 3.5f, 0.0f, 1.0f);
            auto alpha = static_cast<std::uint32_t>(std::lround(coverage * 255));
            for (int column = 0; column < columns; ++column)
                result[y * width + column * tileSize + x] = (colors[column] & 0x00ffffff) | (alpha << 24);
        }
        return result;
    }
};
} // namespace wayfinder
