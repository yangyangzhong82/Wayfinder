#include "wayfinder/MapArchive.h"
#include <stdexcept>
#include <chrono>
#include <utility>

namespace wayfinder {
std::filesystem::path MapArchive::tilePath(TileKey key) const {
    return mDirectory
         / (std::to_string(key.dimension) + "_" + std::to_string(key.x) + "_" + std::to_string(key.z)
            + (key.layer().underground() ? "_c" + std::to_string(key.slice) : "") + ".wfmap");
}
MapArchive::MapArchive(std::filesystem::path legacyPath, std::string identity, std::size_t capacity,
                       std::shared_ptr<ArchiveLoadProgress> progress)
: mDirectory(legacyPath),
  mIdentity(std::move(identity)),
  mCapacity(std::max<std::size_t>(1, capacity)) {
    if (progress) {
        progress->phase.store(ArchiveLoadProgress::Phase::Scanning);
        progress->completed.store(0);
        progress->total.store(0);
    }
    mDirectory += ".tiles";
    std::filesystem::create_directories(mDirectory);
    auto marker = mDirectory / "identity.wfmap";
    if (std::filesystem::exists(marker)) readMap(marker, mIdentity, 0);
    std::vector<std::filesystem::path> files;
    for (auto const& file : std::filesystem::directory_iterator(mDirectory)) {
        if (!file.is_regular_file() || file.path().extension() != ".wfmap" || file.path() == marker) continue;
        files.push_back(file.path());
        if (progress) progress->total.store(files.size());
    }
    if (progress) progress->phase.store(ArchiveLoadProgress::Phase::Reading);
    for (auto const& file : files) {
        try {
            auto records = readMap(file, mIdentity, 2, &mMaterials);
            if (records.size() != 1 || file.filename() != tilePath(records.front().key).filename())
                throw std::runtime_error("Invalid history tile");
            index(records.front().key, records.front().tile);
        } catch (std::exception const& ex) {
            reject(file, ex.what());
        }
        if (progress) progress->completed.fetch_add(1);
    }
    if (!std::filesystem::exists(marker)) {
        // Import once, retaining the original snapshot. Interrupted imports resume
        // without overwriting tiles already committed by an earlier attempt.
        if (progress) progress->phase.store(ArchiveLoadProgress::Phase::Migrating);
        std::vector<TileRecord> legacy;
        try {
            legacy = readMap(legacyPath, mIdentity, 65536, &mMaterials);
        } catch (std::exception const& ex) {
            // Keep a bad legacy snapshot in place; valid per-tile history remains usable.
            mWarnings.push_back("Legacy snapshot preserved: " + legacyPath.string() + ": " + ex.what());
        }
        for (auto const& record : legacy)
            if (!mIndex.contains(record.key)) update({record});
        flush();
        writeMap(marker, mIdentity, {});
    }
    if (progress) progress->phase.store(ArchiveLoadProgress::Phase::Ready);
}
void MapArchive::reject(std::filesystem::path const& path, std::string const& reason) {
    ++mRejected;
    auto backup = path;
    backup += ".corrupt-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count())
        + "-" + std::to_string(mRejected);
    std::error_code error;
    // Renaming preserves the bytes and keeps the file out of subsequent tile scans.
    std::filesystem::rename(path, backup, error);
    bool missing = error == std::errc::no_such_file_or_directory;
    if (error && !missing) mBlockedFiles.insert(path.filename().string());
    if (mWarnings.size() < 20)
        mWarnings.push_back(path.string() + ": " + reason + (missing ? " (missing; fresh samples may recreate it)"
            : error ? " (preserved in place; writes blocked)" : " (isolated as " + backup.string() + ")"));
}
std::vector<std::string> MapArchive::takeWarnings() { return std::exchange(mWarnings, {}); }
void MapArchive::rebuildBounds() {
    mBounds.clear();
    for (auto const& [key, entry] : mIndex) {
        if (!entry.overview.count) continue;
        auto [it, inserted] = mBounds.try_emplace(key.layer(), entry.bounds);
        if (inserted) continue;
        auto& b = it->second;
        b.minX = std::min(b.minX, entry.bounds.minX);
        b.minZ = std::min(b.minZ, entry.bounds.minZ);
        b.maxX = std::max(b.maxX, entry.bounds.maxX);
        b.maxZ = std::max(b.maxZ, entry.bounds.maxZ);
    }
}
void MapArchive::index(TileKey key, MapTile const& tile) {
    Entry entry{tileOverview(tile), {}, tile.lastTouched, tile.biomes.overview(), {}};
    TerrainNeighborhood heights(tile);
    bool  first = true;
    for (int i = 0; i < 256; ++i) {
        if (!tile.cells[i].known()) continue;
        auto cell = tile.cells[i];
        auto color = shadePixel(cell.color, terrainShadeAt(cell, heights, i % 16, i / 16));
        for (int dark = 0; dark < 16; ++dark) entry.lighting[dark].add(litColor(color, cell, {true, dark}));
        double x = key.x * 16.0 + i % 16, z = key.z * 16.0 + i / 16;
        if (first) {
            entry.bounds = {x, z, x + 1, z + 1};
            first        = false;
        } else {
            entry.bounds.minX = std::min(entry.bounds.minX, x);
            entry.bounds.minZ = std::min(entry.bounds.minZ, z);
            entry.bounds.maxX = std::max(entry.bounds.maxX, x + 1);
            entry.bounds.maxZ = std::max(entry.bounds.maxZ, z + 1);
        }
    }
    mIndex.insert_or_assign(key, entry);
    if (entry.overview.count != 0) {
        auto [it, inserted] = mBounds.try_emplace(key.layer(), entry.bounds);
        if (!inserted) {
            auto& b = it->second;
            b.minX  = std::min(b.minX, entry.bounds.minX);
            b.minZ  = std::min(b.minZ, entry.bounds.minZ);
            b.maxX  = std::max(b.maxX, entry.bounds.maxX);
            b.maxZ  = std::max(b.maxZ, entry.bounds.maxZ);
        }
    }
}
void MapArchive::store(TileKey key, Resident& value) {
    if (!value.dirty) return;
    if (mBlockedFiles.contains(tilePath(key).filename().string()))
        throw std::runtime_error("Cannot replace an unquarantined history tile: " + tilePath(key).string());
    writeMap(
        tilePath(key),
        mIdentity,
        {
            {key, value.tile}
    }
    );
    value.dirty = false;
}
MapArchive::Resident& MapArchive::load(TileKey key) {
    if (auto it = mResident.find(key); it != mResident.end()) {
        it->second.accessed = ++mAccess;
        return it->second;
    }
    if (mResident.size() >= mCapacity) {
        auto oldest = std::min_element(mResident.begin(), mResident.end(), [](auto const& a, auto const& b) {
            return a.second.accessed < b.second.accessed;
        });
        store(oldest->first, oldest->second);
        mResident.erase(oldest);
    }
    MapTile tile;
    if (mIndex.contains(key)) {
        try {
            auto records = readMap(tilePath(key), mIdentity, 2, &mMaterials);
            if (records.size() != 1 || records.front().key != key)
                throw std::runtime_error("Missing or invalid history tile");
            tile = records.front().tile;
        } catch (std::exception const& ex) {
            reject(tilePath(key), ex.what());
            mIndex.erase(key);
            rebuildBounds();
        }
    }
    return mResident.emplace(key, Resident{tile, ++mAccess, false}).first->second;
}
void MapArchive::update(std::vector<TileRecord> const& records) {
    for (auto const& record : records) {
        auto& resident = load(record.key);
        // A newly sampled tile can be partial after RAM eviction. Merge only known
        // cells so that it cannot erase the rest of the archived chunk.
        for (int i = 0; i < 256; ++i) {
            auto incoming = record.tile.cells[i];
            retainPendingMaterial(incoming, resident.tile.cells[i]);
            if (incoming.known() && resident.tile.cells[i] != incoming) {
                resident.tile.cells[i] = std::move(incoming);
                resident.dirty         = true;
            }
            if (record.tile.cells[i].known() && resident.tile.biomes.set(i, record.tile.biomes.get(i)))
                resident.dirty = true;
        }
        if (record.tile.lastTouched > resident.tile.lastTouched) {
            resident.tile.lastTouched = record.tile.lastTouched;
            resident.dirty            = true;
        }
        index(record.key, resident.tile);
    }
}
void MapArchive::flush() {
    for (auto& [key, value] : mResident) store(key, value);
}
std::vector<TileRecord> MapArchive::recent(std::size_t count) {
    std::vector<TileKey> keys;
    for (auto const& [key, entry] : mIndex) keys.push_back(key);
    std::sort(keys.begin(), keys.end(), [&](auto a, auto b) { return mIndex.at(a).touched > mIndex.at(b).touched; });
    std::vector<TileRecord> result;
    for (std::size_t i = 0; i < std::min(count, keys.size()); ++i) result.push_back({keys[i], load(keys[i]).tile});
    return result;
}
std::unordered_map<MapLayer, MapBounds, MapLayerHash> MapArchive::bounds() const { return mBounds; }
MapCell MapArchive::get(MapLayer layer, int x, int z) {
    TileKey key{layer.dimension, floorDiv(x, 16), floorDiv(z, 16), layer.slice};
    if (!mIndex.contains(key)) return {};
    return load(key).tile.cells[localBlock(z) * 16 + localBlock(x)];
}

std::string MapArchive::biome(MapLayer layer, int x, int z) {
    TileKey key{layer.dimension, floorDiv(x, 16), floorDiv(z, 16), layer.slice};
    if (!mIndex.contains(key)) return {};
    return std::string(load(key).tile.biomes.get(localBlock(z) * 16 + localBlock(x)));
}
BiomeMap MapArchive::biomes(MapLayer layer, MapView const& view) {
    BiomeRaster raster(view);
    std::vector<TileKey> visible;
    for (auto const& [key, entry] : mIndex)
        if (key.layer() == layer && raster.intersects(key)) {
            if (raster.coarse()) raster.uniform(key, entry.biome);
            else visible.push_back(key);
        }
    for (auto key : visible) if (mIndex.contains(key)) raster.tile(key, load(key).tile.biomes);
    return raster.finish();
}

std::vector<std::uint32_t> MapArchive::rasterize(MapLayer layer, MapView const& view, MapLighting lighting) {
    MapRaster raster(view, lighting);
    // Loading can isolate a tile (including a north neighbour), invalidating index entries.
    std::vector<TileKey> visible;
    for (auto const& [key, entry] : mIndex) {
        if (key.layer() != layer || !raster.intersects(key)) continue;
        visible.push_back(key);
    }
    for (auto key : visible) {
        auto found = mIndex.find(key);
        if (found == mIndex.end()) continue;
        if (view.blocksPerPixel >= 32) raster.add(key.x * 16.0, key.z * 16.0, 16,
            lighting.enabled ? found->second.lighting[std::clamp(lighting.skyDarken, 0, 15)] : found->second.overview);
        else {
            // Own the center snapshot before any neighbour load: capacity can
            // be one. The halo copies heights only, never engine/material data.
            auto tile = load(key).tile;
            TerrainNeighborhood heights(tile);
            for (int direction = 0; direction < 4; ++direction) {
                auto offset = TerrainNeighborhood::directions[direction];
                TileKey neighbor{key.dimension, key.x + offset[0], key.z + offset[1], key.slice};
                if (mIndex.contains(neighbor)) heights.edge(direction, load(neighbor).tile);
            }
            raster.tile(key, tile, heights);
        }
    }
    return raster.finish();
}
} // namespace wayfinder
