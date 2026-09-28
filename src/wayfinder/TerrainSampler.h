#pragma once
#include "wayfinder/MapCore.h"
#include "wayfinder/Settings.h"
#include <deque>
#include <unordered_set>

class BlockSource;
namespace wayfinder {
class TerrainSampler {
public:
    void reset();
    void markDirty(int dimension, int blockX, int blockZ);
    void tick(BlockSource& source, int dimension, int playerX, int playerZ, MapCache& cache, Settings const& settings);

private:
    static std::optional<MapCell>            sample(BlockSource& source, int x, int z, Settings const& settings);
    std::vector<TileKey>                     mSweep;
    std::size_t                              mSweepIndex{};
    std::optional<TileKey>                   mCurrent;
    int                                      mColumn{};
    std::optional<TileKey>                   mCenter;
    std::deque<TileKey>                      mDirty;
    std::unordered_set<TileKey, TileKeyHash> mDirtySet;
};
} // namespace wayfinder
