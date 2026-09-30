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
        mNear.clear();
        mDirty.clear();
        mDirtySet.clear();
        mCenter.reset();
        mLastDirty = false;
        mNearTurns = 0;
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
        // A small local ring refreshes lighting/building changes promptly instead
        // of waiting for the entire (up to 33x33 chunk) discovery sweep.
        mNear.clear();
        for (int ring = 0; ring <= std::min(radius, 2); ++ring)
            for (int z = -ring; z <= ring; ++z)
                for (int x = -ring; x <= ring; ++x)
                    if (std::max(std::abs(x), std::abs(z)) == ring)
                        mNear.push_back({center.dimension, center.x + x, center.z + z, center.slice});
        mNearTurns = 0;
    }
    void dirty(TileKey key) {
        if (!contains(key)) return;
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
        // Two local chunks for every full-sweep chunk. Dirty work still alternates
        // with regular work, so neither a busy farm nor local refresh starves discovery.
        if (!mNear.empty() && mNearTurns++ < 2) {
            auto key = mNear.front();
            mNear.pop_front();
            mNear.push_back(key);
            mLastDirty = false;
            return key;
        }
        mNearTurns = 0;
        auto key = mSweep.front();
        mSweep.pop_front();
        mSweep.push_back(key);
        mLastDirty = false;
        return key;
    }

private:
    std::deque<TileKey>                      mSweep, mNear, mDirty;
    std::unordered_set<TileKey, TileKeyHash> mDirtySet;
    std::optional<TileKey>                   mCenter;
    int                                      mRadius{};
    bool                                     mLastDirty{};
    int                                      mNearTurns{};
};
} // namespace wayfinder
