#pragma once
#include "wayfinder/MapStorage.h"
#include "wayfinder/BiomeOverlay.h"
#include <atomic>
#include <memory>

namespace wayfinder {
struct ArchiveLoadProgress {
    enum class Phase { Scanning, Reading, Migrating, Ready };
    std::atomic<Phase> phase{Phase::Scanning};
    std::atomic<std::size_t> completed{}, total{};
};
// Used only by one I/O job at a time. The index retains small summaries; detailed
// tiles are bounded independently of the number of explored tiles on disk.
class MapArchive {
public:
    MapArchive(std::filesystem::path legacyPath, std::string identity, std::size_t capacity,
               std::shared_ptr<ArchiveLoadProgress> progress = {});
    void                               update(std::vector<TileRecord> const& records);
    void                               flush();
    std::vector<TileRecord>            recent(std::size_t count);
    MapCell                            get(MapLayer layer, int x, int z);
    std::string                        biome(MapLayer layer, int x, int z);
    BiomeMap                           biomes(MapLayer layer, MapView const& view);
    std::vector<std::uint32_t>         rasterize(MapLayer layer, MapView const& view, MapLighting lighting = {});
    std::unordered_map<MapLayer, MapBounds, MapLayerHash> bounds() const;
    std::size_t                        size() const { return mIndex.size(); }
    std::size_t                        rejectedTiles() const { return mRejected; }
    std::vector<std::string>            takeWarnings();

private:
    struct Entry {
        ColorAggregate overview;
        MapBounds      bounds;
        std::uint64_t  touched{};
        std::string    biome;
        std::array<ColorAggregate, 16> lighting;
    };
    struct Resident {
        MapTile       tile;
        std::uint64_t accessed{};
        bool          dirty{};
    };
    std::filesystem::path                              tilePath(TileKey key) const;
    Resident&                                          load(TileKey key);
    void                                               index(TileKey key, MapTile const& tile);
    void                                               store(TileKey key, Resident& value);
    void                                               reject(std::filesystem::path const& path, std::string const& reason);
    void                                               rebuildBounds();
    std::filesystem::path                              mDirectory;
    std::string                                        mIdentity;
    std::size_t                                        mCapacity;
    std::uint64_t                                      mAccess{};
    std::unordered_map<TileKey, Entry, TileKeyHash>    mIndex;
    std::unordered_map<MapLayer, MapBounds, MapLayerHash> mBounds;
    std::unordered_map<TileKey, Resident, TileKeyHash> mResident;
    std::unordered_set<std::string>                    mBlockedFiles;
    std::vector<std::string>                           mWarnings;
    std::size_t                                       mRejected{};
    TerrainMaterialPool                               mMaterials;
};
} // namespace wayfinder
