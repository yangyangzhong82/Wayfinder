#pragma once
#include "wayfinder/MapCore.h"

namespace wayfinder {
struct BiomeRegionLabel {
    std::uint16_t biome{};
    double x{}, z{};
    std::size_t area{};
};
struct BiomeMap {
    MapView view;
    int width{}, height{};
    std::vector<std::uint16_t> cells;
    std::vector<std::string> names;
    std::vector<BiomeRegionLabel> labels;
    std::array<double, 2> worldAt(double x, double z) const {
        return view.worldAt(x * view.width / width, z * view.height / height);
    }
    std::uint16_t at(int x, int z) const {
        return x >= 0 && z >= 0 && x < width && z < height ? cells[z * width + x] : 0;
    }
    std::uint16_t atWorld(double x, double z) const {
        if (cells.empty()) return 0;
        auto origin = view.worldAt(0, 0);
        return at(blockCoordinate((x - origin[0]) / (view.width * view.blocksPerPixel) * width),
                  blockCoordinate((z - origin[1]) / (view.height * view.blocksPerPixel) * height));
    }
};
// Bounded grid shared by live and disk maps. Labels follow connected regions,
// rather than averaging disconnected islands into an unrelated biome.
class BiomeRaster {
public:
    explicit BiomeRaster(MapView view) {
        mMap.view = view;
        mMap.width = std::clamp(view.width, 1, 512);
        mMap.height = std::clamp(view.height, 1, 512);
        mMap.cells.resize(mMap.width * mMap.height);
    }
    bool coarse() const {
        return std::min(mMap.view.width * mMap.view.blocksPerPixel / mMap.width,
                        mMap.view.height * mMap.view.blocksPerPixel / mMap.height) >= 16;
    }
    bool intersects(TileKey key) const {
        auto origin = mMap.view.worldAt(0, 0), end = mMap.view.worldAt(mMap.view.width, mMap.view.height);
        return key.x * 16.0 < end[0] && (key.x + 1) * 16.0 > origin[0]
            && key.z * 16.0 < end[1] && (key.z + 1) * 16.0 > origin[1];
    }
    void tile(TileKey key, BiomeTile const& tile) {
        if (coarse()) uniform(key, tile.overview());
        else add(key, [&](int x, int z) { return tile.get(localBlock(z) * 16 + localBlock(x)); });
    }
    void uniform(TileKey key, std::string_view name) {
        if (!name.empty()) add(key, [&](int, int) { return name; });
    }
    BiomeMap finish() {
        std::vector<bool> visited(mMap.cells.size());
        std::vector<int> pending;
        for (int start = 0; start < int(mMap.cells.size()); ++start) {
            auto id = mMap.cells[start];
            if (!id || visited[start]) continue;
            pending.clear(); pending.push_back(start); visited[start] = true;
            double sumX = 0, sumZ = 0;
            for (std::size_t next = 0; next < pending.size(); ++next) {
                int index = pending[next], x = index % mMap.width, z = index / mMap.width;
                sumX += x; sumZ += z;
                auto visit = [&](int nx, int nz) {
                    if (mMap.at(nx, nz) != id) return;
                    int cell = nz * mMap.width + nx;
                    if (!visited[cell]) { visited[cell] = true; pending.push_back(cell); }
                };
                visit(x - 1, z); visit(x + 1, z); visit(x, z - 1); visit(x, z + 1);
            }
            if (pending.size() < 24) continue;
            double cx = sumX / pending.size(), cz = sumZ / pending.size();
            int anchor = start;
            double distance = std::numeric_limits<double>::max();
            for (int index : pending) {
                double dx = index % mMap.width - cx, dz = index / mMap.width - cz;
                if (dx * dx + dz * dz < distance) { distance = dx * dx + dz * dz; anchor = index; }
            }
            auto world = mMap.worldAt(anchor % mMap.width + 0.5, anchor / mMap.width + 0.5);
            mMap.labels.push_back({id, world[0], world[1], pending.size()});
        }
        std::sort(mMap.labels.begin(), mMap.labels.end(), [](auto const& a, auto const& b) { return a.area > b.area; });
        if (mMap.labels.size() > 64) mMap.labels.resize(64);
        return std::move(mMap);
    }
private:
    template <class Read> void add(TileKey key, Read&& read) {
        if (!intersects(key)) return;
        auto origin = mMap.view.worldAt(0, 0);
        double dx = mMap.view.width * mMap.view.blocksPerPixel / mMap.width;
        double dz = mMap.view.height * mMap.view.blocksPerPixel / mMap.height;
        auto edge = [](double value, int maximum) { return int(std::clamp(std::ceil(value - 0.5), 0.0, double(maximum))); };
        int left = edge((key.x * 16.0 - origin[0]) / dx, mMap.width);
        int right = edge(((key.x + 1) * 16.0 - origin[0]) / dx, mMap.width);
        int top = edge((key.z * 16.0 - origin[1]) / dz, mMap.height);
        int bottom = edge(((key.z + 1) * 16.0 - origin[1]) / dz, mMap.height);
        for (int z = top; z < bottom; ++z) for (int x = left; x < right; ++x) {
            auto world = mMap.worldAt(x + 0.5, z + 0.5);
            auto name = read(blockCoordinate(world[0]), blockCoordinate(world[1]));
            if (name.empty()) continue;
            if (name == mLastName) { mMap.cells[z * mMap.width + x] = mLastId; continue; }
            auto found = mPalette.find(std::string(name));
            if (found == mPalette.end()) {
                if (mMap.names.size() >= 4096) continue;
                mMap.names.emplace_back(name);
                found = mPalette.emplace(mMap.names.back(), static_cast<std::uint16_t>(mMap.names.size())).first;
            }
            mMap.cells[z * mMap.width + x] = found->second;
            mLastName = name;
            mLastId = found->second;
        }
    }
    BiomeMap mMap;
    std::unordered_map<std::string, std::uint16_t> mPalette;
    std::string mLastName;
    std::uint16_t mLastId{};
};
inline BiomeMap rasterizeBiomes(MapCache const& cache, MapLayer layer, MapView view) {
    BiomeRaster raster(view);
    cache.visitTiles(layer, [&](TileKey key, MapTile const& tile) { raster.tile(key, tile.biomes); });
    return raster.finish();
}
inline void overlayBiomes(std::vector<std::uint32_t>& pixels, MapView const& view, BiomeMap const& biomes) {
    if (biomes.cells.empty() || biomes.view != view || pixels.size() != std::size_t(view.width) * view.height) return;
    std::vector<std::uint32_t> colors{0};
    for (auto const& name : biomes.names) {
        std::uint32_t hash = 2166136261u;
        for (unsigned char c : name) hash = (hash ^ c) * 16777619u;
        colors.push_back(rgba(80 + (hash & 127u), 80 + ((hash >> 8) & 127u), 80 + ((hash >> 16) & 127u)));
    }
    for (int z = 0; z < view.height; ++z) for (int x = 0; x < view.width; ++x) {
        int bx = std::min(biomes.width - 1, int((x + 0.5) * biomes.width / view.width));
        int bz = std::min(biomes.height - 1, int((z + 0.5) * biomes.height / view.height));
        auto id = biomes.at(bx, bz);
        if (!id) continue;
        auto east = biomes.at(bx + 1, bz), south = biomes.at(bx, bz + 1);
        // Unknown/fog edges are not biome boundaries.
        bool boundary = (east && east != id) || (south && south != id);
        auto color = boundary ? rgba(255, 225, 150) : colors[id];
        auto& pixel = pixels[z * view.width + x];
        unsigned alpha = boundary ? 7 : 4; // Stronger region fill while retaining 60% of terrain detail.
        auto channel = [&](unsigned shift) {
            return (((pixel >> shift) & 255u) * (10 - alpha) + ((color >> shift) & 255u) * alpha) / 10;
        };
        pixel = rgba(channel(0), channel(8), channel(16));
    }
}
} // namespace wayfinder
