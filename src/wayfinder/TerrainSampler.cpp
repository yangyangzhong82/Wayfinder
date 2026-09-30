#include "wayfinder/TerrainSampler.h"
#include "wayfinder/TerrainColumn.h"

#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/biome/Biome.h"
#include "mc/world/level/biome/biome_color_sampling/BiomeColorSampling.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/chunk/SubChunk.h"
#include "mc/world/level/material/Material.h"
#include "mc/deps/core/string/HashedString.h"
#include <chrono>

namespace wayfinder {
void TerrainSampler::reset() {
    mTextures.reset();
    mSchedule.clear();
    mCurrent.reset();
    mLayer.reset();
    mColumn = 0;
}
void TerrainSampler::markDirty(int dimension, int blockX, int blockZ) {
    if (mLayer && mLayer->dimension == dimension)
        // A light source affects neighbours up to 15 blocks away, across chunk seams.
        for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx)
            mSchedule.dirty({dimension, floorDiv(blockX, 16) + dx, floorDiv(blockZ, 16) + dz, mLayer->slice});
}

namespace {
bool isWater(Block const& block) { return block.getMaterial().mType == SharedTypes::v1_26_20::MaterialType::Water; }
bool isLava(Block const& block) { return block.getMaterial().mType == SharedTypes::v1_26_20::MaterialType::Lava; }
bool readyAt(LevelChunk& chunk, int y) {
    auto sub = chunk.getSubChunk(static_cast<short>(floorDiv(y, 16)));
    if (!sub || sub->isPlaceHolderSubChunk() || !sub->mIsInitialized) return false;
    auto state = sub->mSubChunkState;
    return state == SubChunk::SubChunkState::Normal || state == SubChunk::SubChunkState::ProcessedSubChunk
        || state == SubChunk::SubChunkState::RequestFinished;
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
    else if (
        name == "minecraft:oak_leaves" || name == "minecraft:jungle_leaves" || name == "minecraft:acacia_leaves"
        || name == "minecraft:dark_oak_leaves" || name == "minecraft:mangrove_leaves" || name == "minecraft:vine"
    )
        tint = &getMapDefaultFoliageColor;
    if (!tint) return {};
    return fromRgb(tint(source.getBiome(pos), pos));
}
bool usable(mce::Color const& color) {
    return std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b) && color.a > 0.0f;
}
} // namespace

MapLayer TerrainSampler::selectLayer(int dimension, int y, Settings const& settings, MapLayer previous) {
    return terrainLayer(dimension, y, settings.caveSwitchY, settings.terrainMode, previous);
}

std::optional<MapCell>
TerrainSampler::sample(BlockSource& source, MapLayer layer, int x, int z, Settings const& settings) {
    return sampleWithTextures(source, layer, x, z, settings, nullptr);
}
std::optional<MapCell> TerrainSampler::sampleWithTextures(
    BlockSource& source, MapLayer layer, int x, int z, Settings const& settings, TerrainTextureColors* textures
) {
    if (static_cast<int>(source.getDimensionId()) != layer.dimension) return {};
    auto chunk = source.getChunk(floorDiv(x, 16), floorDiv(z, 16));
    if (!chunk || chunk->mLoadState->load(std::memory_order_acquire) != ChunkState::Loaded) return {};
    int minY = std::max<int>(source.getMinHeight(), -32768);
    int maxY = std::min<int>(source.getMaxHeight(), 32768);
    if (minY >= maxY) return {};
    auto read = [&](int y) -> std::optional<ColumnBlock> {
        if (!readyAt(*chunk, y)) return {};
        BlockPos    pos{x, y, z};
        auto const& block = source.getBlock(pos);
        auto const& name  = block.getTypeName();
        if (name == "minecraft:client_request_placeholder_block") return {};
        auto const& liquid = source.getLiquidBlock(pos);
        if (isWater(block) || isWater(liquid)) {
            if (settings.includeWater) {
                int tint = source.getBiome(pos).mMapWaterColor & 0xffffff;
                return ColumnBlock{ColumnKind::Water, fromRgb(tint ? tint : 0x3f76e4), !isWater(block)};
            }
            if (isWater(block)) return ColumnBlock{};
        }
        if (isLava(block) || isLava(liquid))
            return ColumnBlock{ColumnKind::Lava, rgba(255, 100, 18)};
        if (block.isAir() || (!settings.includeLeaves && name.ends_with("_leaves"))) return ColumnBlock{};
        auto tinted = biomeTint(source, pos, name);
        if (textures) {
            auto top = textures->top(block, pos);
            if (top.material) {
                auto tint = tinted.value_or(0xffffffffu);
                return ColumnBlock{ColumnKind::Solid, tintTopTexture(top.material->average(), tint), false,
                    std::move(top.material), tint, top.rotation, block.mSerializationIdHash};
            }
        }
        if (tinted) return ColumnBlock{ColumnKind::Solid, *tinted, false, {}, *tinted, 0, block.mSerializationIdHash};
        auto color = block.getBlockType().getMapColor(source, pos, block);
        if (!usable(color)) color = *block.getBlockType().mMapColor; // Static per-type colour as fallback.
        if (!usable(color)) return ColumnBlock{};
        return ColumnBlock{ColumnKind::Solid, toPixel(color), false, {}, 0xffffffffu, 0, block.mSerializationIdHash};
    };
    auto illuminate = [&](std::optional<MapCell> cell) {
        if (!cell || !cell->floor()) return cell;
        // Read the air/liquid above the mapped floor, not the opaque floor's zero skylight.
        int y = int(cell->height) + 1;
        if (y >= maxY || !readyAt(*chunk, y)) return cell;
        auto light = source.getBrightnessPair(BlockPos{x, y, z});
        auto floorLight = source.getBrightnessPair(BlockPos{x, cell->height, z});
        cell->skyLight = static_cast<std::uint8_t>(std::min<int>(15, light.sky->mValue));
        cell->blockLight = static_cast<std::uint8_t>(std::min<int>(15,
            std::max(light.block->mValue, floorLight.block->mValue)));
        return cell;
    };
    if (layer.underground()) return illuminate(caveColumn(layer.referenceY(), minY, maxY, read));
    int top = source.getAboveTopSolidBlock(x, z, settings.includeWater, settings.includeLeaves) - 1;
    if (top >= maxY) return {};
    if (top < minY) {
        // The End's empty columns are explored void only when every section is
        // actually available. Incomplete client chunks retain the fog pattern.
        for (int y = minY; y < maxY; y = (floorDiv(y, 16) + 1) * 16)
            if (!readyAt(*chunk, y)) return {};
        return illuminate(surfaceColumn(minY - 1, minY, maxY, read));
    }
    return illuminate(surfaceColumn(top, minY, maxY, read));
}

std::string TerrainSampler::biome(BlockSource& source, MapLayer layer, int x, int z, Settings const& settings,
                                  std::optional<MapCell> sampled) {
    if (static_cast<int>(source.getDimensionId()) != layer.dimension) return {};
    auto chunk = source.getChunk(floorDiv(x, 16), floorDiv(z, 16));
    if (!chunk || chunk->mLoadState->load(std::memory_order_acquire) != ChunkState::Loaded) return {};
    int y = layer.underground() ? layer.referenceY() : 0;
    if (!layer.underground()) {
        auto cell = sampled ? sampled : sample(source, layer, x, z, settings);
        if (!cell || !cell->floor()) return {};
        y = cell->height;
    }
    if (y < source.getMinHeight() || y >= source.getMaxHeight() || !readyAt(*chunk, y)) return {};
    return source.getBiome(BlockPos{x, y, z}).mHash->getString();
}

void TerrainSampler::tick(
    BlockSource&    source,
    MapLayer        layer,
    int             playerX,
    int             playerZ,
    MapCache&       cache,
    Settings const& settings,
    mce::TextureGroup* textures
) {
    if (static_cast<int>(source.getDimensionId()) != layer.dimension) return;
    if (mLayer != layer) reset();
    mTextures.beginTick(textures);
    mLayer = layer;
    TileKey center{layer.dimension, floorDiv(playerX, 16), floorDiv(playerZ, 16), layer.slice};
    int     minimapRadius =
        static_cast<int>(std::ceil(settings.minimapPixels * settings.minimapBlocksPerPixel * 0.5 / 16.0)) + 1;
    int radius = std::max(settings.sampleRadiusChunks, std::min(minimapRadius, 16));
    mSchedule.recenter(center, radius, [&](TileKey key) { return cache.complete(key); });
    if (mCurrent && !mSchedule.contains(*mCurrent)) mCurrent.reset();
    auto end       = std::chrono::steady_clock::now() + std::chrono::microseconds(settings.samplingBudgetMicros);
    int  inspected = 0;
    while (inspected < settings.columnsPerTick && std::chrono::steady_clock::now() < end) {
        if (!mCurrent) {
            mCurrent = mSchedule.next();
            if (!mCurrent) break;
            mColumn = 0;
            if (mCurrent->layer() != layer || !source.getChunk(mCurrent->x, mCurrent->z)) {
                mCurrent.reset();
                ++inspected;
                continue;
            }
        }
        int x = mCurrent->x * 16 + mColumn % 16;
        int z = mCurrent->z * 16 + mColumn / 16;
        if (auto cell = sampleWithTextures(source, layer, x, z, settings, &mTextures)) {
            retainPendingMaterial(*cell, cache.get(layer, x, z));
            cache.put(layer, x, z, *cell, biome(source, layer, x, z, settings, cell));
        }
        ++inspected;
        if (++mColumn == 256) mCurrent.reset();
    }
}
} // namespace wayfinder
