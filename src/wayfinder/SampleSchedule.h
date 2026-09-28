#pragma once
#include "wayfinder/MapCore.h"
#include <deque>
#include <unordered_set>

namespace wayfinder {
// Engine-independent queue: moving the centre never restarts overlapping work.
class SampleSchedule {
public:
    void clear() {
        mSweep.clear();
        mDirty.clear();
        mDirtySet.clear();
        mCenter.reset();
        mLastDirty = false;
    }
    bool contains(TileKey key) const {
        return mCenter && key.layer() == mCenter->layer() && std::abs(key.x - mCenter->x) <= mRadius
            && std::abs(key.z - mCenter->z) <= mRadius;
    }
    template <class Known>
    void recenter(TileKey center, int radius, Known known) {
        if (mCenter && mCenter->layer() != center.layer()) clear();
        if (mCenter && *mCenter == center && mRadius == radius) return;
        mCenter = center;
        mRadius = radius;
        std::erase_if(mSweep, [&](auto key) { return !contains(key); });
        std::erase_if(mDirty, [&](auto key) {
            if (contains(key)) return false;
            mDirtySet.erase(key);
            return true;
        });
        std::unordered_set<TileKey, TileKeyHash> queued(mSweep.begin(), mSweep.end());
        for (int ring = 0; ring <= radius; ++ring)
            for (int z = -ring; z <= ring; ++z)
                for (int x = -ring; x <= ring; ++x) {
                    if (std::max(std::abs(x), std::abs(z)) != ring) continue;
                    TileKey key{center.dimension, center.x + x, center.z + z, center.slice};
                    if (queued.insert(key).second) mSweep.push_back(key);
                }
        std::stable_partition(mSweep.begin(), mSweep.end(), [&](auto key) { return !known(key); });
    }
    void dirty(TileKey key) {
        if (mDirty.size() < 2048 && mDirtySet.insert(key).second) mDirty.push_back(key);
    }
    std::optional<TileKey> next() {
        // Alternate dirty work and discovery, so a busy farm cannot starve exploration.
        if (!mDirty.empty() && (!mLastDirty || mSweep.empty())) {
            auto key = mDirty.front();
            mDirty.pop_front();
            mDirtySet.erase(key);
            mLastDirty = true;
            return key;
        }
        if (mSweep.empty()) return {};
        auto key = mSweep.front();
        mSweep.pop_front();
        mSweep.push_back(key);
        mLastDirty = false;
        return key;
    }

private:
    std::deque<TileKey>                      mSweep, mDirty;
    std::unordered_set<TileKey, TileKeyHash> mDirtySet;
    std::optional<TileKey>                   mCenter;
    int                                      mRadius{};
    bool                                     mLastDirty{};
};
} // namespace wayfinder
