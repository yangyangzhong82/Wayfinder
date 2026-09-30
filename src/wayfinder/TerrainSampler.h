#pragma once
#include "wayfinder/MapCore.h"
#include "wayfinder/SampleSchedule.h"
#include "wayfinder/Settings.h"
#include "wayfinder/TerrainTextureColor.h"

class BlockSource;
namespace mce { class TextureGroup; }
namespace wayfinder {
class TerrainSampler {
public:
    void reset();
    void markDirty(int dimension, int blockX, int blockZ);
    void tick(BlockSource& source, MapLayer layer, int playerX, int playerZ, MapCache& cache, Settings const& settings,
              mce::TextureGroup* textures = nullptr);
    static MapLayer               selectLayer(int dimension, int y, Settings const& settings, MapLayer previous);
    static std::optional<MapCell> sample(BlockSource& source, MapLayer layer, int x, int z, Settings const& settings);
    static std::string biome(BlockSource& source, MapLayer layer, int x, int z, Settings const& settings,
                             std::optional<MapCell> sampled = {});

private:
    static std::optional<MapCell> sampleWithTextures(BlockSource& source, MapLayer layer, int x, int z,
                                                     Settings const& settings, TerrainTextureColors* textures);
    TerrainTextureColors    mTextures;
    SampleSchedule          mSchedule;
    std::optional<TileKey>  mCurrent;
    std::optional<MapLayer> mLayer;
    int                     mColumn{};
};
} // namespace wayfinder
