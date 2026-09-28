#include "wayfinder/TerrainSampler.h"

#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/biome/Biome.h"
#include "mc/world/level/biome/biome_color_sampling/BiomeColorSampling.h"
#include "mc/world/level/material/Material.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/chunk/SubChunk.h"
#include <chrono>

namespace wayfinder {
void TerrainSampler::reset() {
    mSweep.clear();
    mSweepIndex = 0;
    mCurrent.reset();
    mColumn = 0;
    mCenter.reset();
    mDirty.clear();
    mDirtySet.clear();
}
void TerrainSampler::markDirty(int dimension, int blockX, int blockZ) {
    TileKey key{dimension, floorDiv(blockX, 16), floorDiv(blockZ, 16)};
    if (mDirty.size() < 2048 && mDirtySet.insert(key).second) mDirty.push_back(key);
}

namespace {
constexpr int maxProbe = 64; // Covers deep ocean and tall canopies without unbounded scans.

bool isWater(Block const& block) {
    return block.getMaterial().mType == SharedTypes::v1_26_20::MaterialType::Water;
}
std::uint32_t toPixel(mce::Color const& color) {
    auto byte = [](float value) { return static_cast<unsigned>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return rgba(byte(color.r), byte(color.g), byte(color.b));
}
std::uint32_t fromRgb(int value) {
    auto rgb = static_cast<std::uint32_t>(value);
    return rgba((rgb >> 16) & 255u, (rgb >> 8) & 255u, rgb & 255u);
}
// getMapColor returns the untinted base for biome-coloured blocks (grass looks like dirt,
// leaves look brown), so apply the exported map tint for the common ones.
std::optional<std::uint32_t> biomeTint(BlockSource& source, BlockPos const& pos, std::string const& name) {
    using namespace BiomeColorSampling;
    int (*tint)(Biome const&, BlockPos const&) = nullptr;
    if (name == "minecraft:grass_block" || name == "minecraft:short_grass" || name == "minecraft:tall_grass"
        || name == "minecraft:fern" || name == "minecraft:large_fern")
        tint = &getMapGrassColor;
    else if (name == "minecraft:birch_leaves") tint = &getMapBirchFoliageColor;
    else if (name == "minecraft:spruce_leaves") tint = &getMapEvergreenFoliageColor;
    else if (name == "minecraft:oak_leaves" || name == "minecraft:jungle_leaves" || name == "minecraft:acacia_leaves"
             || name == "minecraft:dark_oak_leaves" || name == "minecraft:mangrove_leaves" || name == "minecraft:vine")
        tint = &getMapDefaultFoliageColor;
    if (!tint) return {};
    return fromRgb(tint(source.getBiome(pos), pos));
}
bool usable(mce::Color const& color) {
    return std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b) && color.a > 0.0f;
}
} // namespace

std::optional<MapCell> TerrainSampler::sample(BlockSource& source, int x, int z, Settings const& settings) {
    auto chunk = source.getChunk(floorDiv(x, 16), floorDiv(z, 16));
    if (!chunk || chunk->mLoadState->load(std::memory_order_acquire) != ChunkState::Loaded) return {};
    int minY = source.getMinHeight(), maxY = source.getMaxHeight();
    int y = source.getAboveTopSolidBlock(x, z, settings.includeWater, settings.includeLeaves) - 1;
    if (y < minY || y >= maxY) return {};
    std::optional<MapCell> water; // Surface of a water column; depth grows until the floor is found.
    // Confirm the data exists instead of treating a client request placeholder as terrain.
    // Walk down past decorations without a map colour and through water to its floor.
    for (int top = y; y >= minY && top - y < maxProbe; --y) {
        auto sub = chunk->getSubChunk(static_cast<short>(floorDiv(y, 16)));
        if (!sub || sub->isPlaceHolderSubChunk() || !sub->mIsInitialized) return water;
        auto state = sub->mSubChunkState;
        if (state != SubChunk::SubChunkState::Normal && state != SubChunk::SubChunkState::ProcessedSubChunk
            && state != SubChunk::SubChunkState::RequestFinished)
            return water;
        BlockPos    pos{x, y, z};
        auto const& block = source.getBlock(pos);
        auto const& name  = block.getTypeName();
        if (name == "minecraft:client_request_placeholder_block") return water;
        // Water blocks and waterlogged blocks both count towards the column depth.
        if (isWater(block) || isWater(source.getLiquidBlock(pos))) {
            if (!water) {
                // Biome water colour is a named field; getMapColor reports water as transparent.
                int tint = source.getBiome(pos).mMapWaterColor & 0xffffff;
                water    = MapCell{fromRgb(tint ? tint : 0x3f76e4), static_cast<std::int16_t>(y)};
            }
            water->depth = static_cast<std::uint8_t>(std::min(water->height - y + 1, 255));
            if (!isWater(block)) return water; // Waterlogged block is the floor.
            continue;
        }
        if (block.isAir()) {
            if (water) return water;
            continue;
        }
        if (water) return water; // Reached the floor.
        if (auto tinted = biomeTint(source, pos, name)) return MapCell{*tinted, static_cast<std::int16_t>(y)};
        auto color = block.getBlockType().getMapColor(source, pos, block);
        if (!usable(color)) color = *block.getBlockType().mMapColor; // Static per-type colour as fallback.
        if (!usable(color)) continue;
        return MapCell{toPixel(color), static_cast<std::int16_t>(y)};
    }
    return water;
}

void TerrainSampler::tick(
    BlockSource&    source,
    int             dimension,
    int             playerX,
    int             playerZ,
    MapCache&       cache,
    Settings const& settings
) {
    TileKey center{dimension, floorDiv(playerX, 16), floorDiv(playerZ, 16)};
    if (!mCenter || *mCenter != center) {
        reset();
        mCenter = center;
        // Near-to-far square rings give immediate feedback around the player.
        // Never sweep less than the minimap shows, plus one chunk for the off-centre player.
        int minimapRadius = static_cast<int>(
            std::ceil(settings.minimapPixels * settings.minimapBlocksPerPixel * 0.5 / 16.0)
        ) + 1;
        int radius = std::max(settings.sampleRadiusChunks, std::min(minimapRadius, 16));
        for (int ring = 0; ring <= radius; ++ring) {
            for (int z = -ring; z <= ring; ++z) {
                for (int x = -ring; x <= ring; ++x) {
                    if (std::max(std::abs(x), std::abs(z)) == ring)
                        mSweep.push_back({dimension, center.x + x, center.z + z});
                }
            }
        }
    }
    auto end       = std::chrono::steady_clock::now() + std::chrono::microseconds(settings.samplingBudgetMicros);
    int  inspected = 0;
    while (inspected < settings.columnsPerTick && std::chrono::steady_clock::now() < end) {
        if (!mCurrent) {
            if (!mDirty.empty()) {
                mCurrent = mDirty.front();
                mDirty.pop_front();
                mDirtySet.erase(*mCurrent);
            } else {
                if (mSweep.empty()) break;
                mCurrent = mSweep[mSweepIndex++ % mSweep.size()];
            }
            mColumn = 0;
            if (mCurrent->dimension != dimension || !source.getChunk(mCurrent->x, mCurrent->z)) {
                mCurrent.reset();
                ++inspected;
                continue;
            }
        }
        int x = mCurrent->x * 16 + mColumn % 16;
        int z = mCurrent->z * 16 + mColumn / 16;
        if (auto cell = sample(source, x, z, settings)) cache.put(dimension, x, z, *cell);
        ++inspected;
        if (++mColumn == 256) mCurrent.reset();
    }
}
} // namespace wayfinder
