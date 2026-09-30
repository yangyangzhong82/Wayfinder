#include "wayfinder/MapArchive.h"
#include "wayfinder/TerrainColumn.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>

using namespace wayfinder;
namespace {
int checks{};
void require(bool value, char const* message) { ++checks; if (!value) throw std::runtime_error(message); }
template <class F> void rejects(F&& f, char const* message) {
    bool rejected = false;
    try { f(); } catch (std::exception const&) { rejected = true; }
    require(rejected, message);
}
struct Directory {
    std::filesystem::path path = std::filesystem::path("build/tests")
        / ("materials-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Directory() { std::filesystem::create_directories(path); }
    ~Directory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
TerrainMaterialPtr quadrants() {
    TerrainMaterial::Pixels pixels;
    for (int z = 0; z < 16; ++z) for (int x = 0; x < 16; ++x)
        pixels[z * 16 + x] = z < 8 ? (x < 8 ? rgba(200, 40, 20) : rgba(40, 180, 20))
                                  : (x < 8 ? rgba(40, 40, 220) : rgba(200, 180, 220));
    return std::make_shared<TerrainMaterial const>(pixels);
}
MapCell ground(TerrainMaterialPtr material) {
    return {material->average(), 64, 0, 0, 15, 0, std::move(material), 0xffffffffu, 0, 0x12345678abcdef01ull};
}
void samplingTests() {
    std::array<std::uint8_t, 32 * 16 * 4> bytes{};
    for (int z = 0; z < 16; ++z) for (int x = 0; x < 32; ++x) {
        auto i = (z * 32 + x) * 4;
        bytes[i] = std::uint8_t(x * 7); bytes[i + 1] = std::uint8_t(z * 13);
        bytes[i + 2] = 20; bytes[i + 3] = 255;
    }
    auto m = sampleTopMaterial(bytes, 32, 16, 4, false, 0.5f, 0, 1, 1);
    require(bool(m), "A valid RGBA top face is sampled");
    for (int z = 0; z < 16; ++z) for (int x = 0; x < 16; ++x)
        require(m->texel(0, (x + 0.5) / 16, (z + 0.5) / 16, 0) == rgba((x + 16) * 7, z * 13, 20),
            "Every source texel survives rather than being replaced with an average");
    auto mirror = sampleTopMaterial(bytes, 32, 16, 4, false, 1, 1, 0.5f, 0);
    require(mirror->texel(0, 0.01, 0.01, 0) == m->texel(0, 0.99, 0.99, 0), "Flipped source UVs preserve orientation");
    auto bgra = sampleTopMaterial(bytes, 32, 16, 4, true, 0.5f, 0, 1, 1);
    require(bgra->texel(0, 0.01, 0.01, 0) == rgba(20, 0, 112), "BGRA channels are decoded correctly");
    std::array<std::uint8_t, 3> rgb{80, 160, 240};
    auto small = sampleTopMaterial(rgb, 1, 1, 3, false, 0, 0, 1, 1);
    require(small && small->average() == rgba(80, 160, 240), "RGB and tiny resource textures remain valid");
    require(!sampleTopMaterial(bytes, 32, 16, 4, false, -0.1f, 0, 1, 1)
        && !sampleTopMaterial(bytes, 32, 16, 4, false, 0, 0, 0, 1)
        && !sampleTopMaterial(bytes, 32, 16, 4, false, 0, 0, std::numeric_limits<float>::quiet_NaN(), 1)
        && !sampleTopMaterial(std::span(bytes).first(7), 32, 16, 4, false, 0, 0, 1, 1),
        "Invalid rectangles, non-finite UVs and truncated buffers fail safely");
    auto uv = sourceTextureUv(0.5f, 0.25f, 0.5625f, 0.3125f, 256, 256, 16, 16);
    require(uv == std::array<float, 4>{0, 0, 1, 1}, "Source images do not accidentally use atlas offsets");
    uv = sourceTextureUv(0.5625f, 0.3125f, 0.5f, 0.25f, 256, 256, 16, 64);
    require(uv == std::array<float, 4>{1, 0.25f, 0, 0}, "Animated source strips select one frame and retain mirrors");
    TerrainMaterial::Pixels transparent{};
    for (int i = 0; i < 128; ++i) transparent[i] = rgba(180, 100, 60);
    auto leaves = std::make_shared<TerrainMaterial const>(transparent);
    require(leaves->average() == rgba(180, 100, 60), "Transparent holes do not add black to mip averages");
    require(materialPixel(0, 0xffffffffu, rgba(100, 110, 120)) == rgba(100, 110, 120), "Transparent pixels fill with terrain, not HUD holes");
    require(materialPixel(rgba(200, 100, 50), rgba(128, 255, 255), 0) == rgba(100, 100, 50), "Biome tint applies once per visible texel");
    bytes.fill(0);
    require(!sampleTopMaterial(bytes, 32, 16, 4, false, 0, 0, 1, 1), "Fully transparent materials fall back safely");
    TerrainMaterial::Pixels checker;
    for (int z = 0; z < 16; ++z) for (int x = 0; x < 16; ++x) checker[z * 16 + x] = (x + z) % 2 ? rgba(240, 240, 240) : rgba(0, 0, 0);
    TerrainMaterial check(checker);
    for (int level = 1; level <= 4; ++level)
        require(check.texel(level, 0.2, 0.7, 0) == rgba(120, 120, 120), "Mipmaps suppress fine checkerboard aliasing");
}
void rasterTests(Directory const& dir) {
    auto material = quadrants();
    MapCache cache;
    auto cell = ground(material);
    for (int z = -17; z < 17; ++z) for (int x = -17; x < 17; ++x) {
        cell.rotation = std::uint8_t(localBlock(x) % 4);
        cell.tint = x < 0 ? rgba(130, 220, 170) : 0xffffffffu;
        cell.color = tintTopTexture(material->average(), cell.tint);
        cache.put(0, x, z, cell);
    }
    MapArchive archive(dir.path / "raster.wfmap", "textures", 1);
    archive.update(cache.snapshot()); archive.flush();
    for (double scale : {0.03125, 0.0625, 0.125, 0.25, 0.3, 0.5, 0.75, 1.0, 2.0, 32.0}) {
        MapView view{-0.13, 0.17, scale, 32, 24};
        for (MapLighting lighting : {MapLighting{}, MapLighting{true, 11}})
            require(cache.rasterize(0, view, lighting) == archive.rasterize(0, view, lighting),
                "Live and persisted material rendering agree across zoom, lighting and negative chunk seams");
    }
    for (unsigned rotation = 0; rotation < 4; ++rotation) {
        MapCache one;
        auto block = ground(material); block.rotation = std::uint8_t(rotation);
        one.put(0, 0, 0, block);
        auto pixels = one.rasterize(0, MapView{0.5, 0.5, 1.0 / 16, 16, 16});
        for (int z = 0; z < 16; ++z) for (int x = 0; x < 16; ++x) {
            int sx = x, sz = z;
            for (unsigned t = 0; t < rotation; ++t) { int old = sx; sx = sz; sz = 15 - old; }
            require(pixels[z * 16 + x] == material->pixels()[sz * 16 + sx], "Rotated top faces render exact source texels");
        }
        auto overview = one.rasterize(0, MapView{0.5, 0.5, 1, 1, 1});
        require(overview[0] == material->average(), "At one pixel per block textures reduce to a stable average");
    }
    for (int offset : {-29999968, 29999968}) {
        MapCache edge;
        edge.put(0, offset, offset, ground(material));
        MapView view{offset + 0.5, offset + 0.5, 1.0 / 16, 16, 16};
        auto pixels = edge.rasterize(0, view);
        require(std::equal(pixels.begin(), pixels.end(), material->pixels().begin()), "World-limit texel coordinates do not drift");
    }
    auto previous = ground(material), pending = previous;
    pending.material.reset(); pending.color = rgba(1, 2, 3); pending.blockLight = 14;
    retainPendingMaterial(pending, previous);
    require(pending.material == material && pending.color == previous.color && pending.blockLight == 14,
        "Pending resource reload keeps old detail without losing new light levels");
    pending.material.reset(); ++pending.surfaceId;
    retainPendingMaterial(pending, previous);
    require(!pending.material, "A replaced block cannot borrow the previous state's texture");
    pending = previous; pending.material.reset(); ++pending.height;
    retainPendingMaterial(pending, previous);
    require(!pending.material, "A different height cannot borrow old detail");
    auto read = [&](int y) -> std::optional<ColumnBlock> {
        return y == 64 ? ColumnBlock{ColumnKind::Solid, previous.color, false, material, rgba(100, 200, 50), 2, 123}
                       : ColumnBlock{};
    };
    auto sampled = surfaceColumn(64, -64, 320, read);
    require(sampled && sampled->material == material && sampled->rotation == 2 && sampled->surfaceId == 123
        && sampled->tint == rgba(100, 200, 50), "Column selection retains the selected floor's material metadata");
    MapCache revision;
    revision.put(0, 0, 0, previous); revision.takeChanges();
    auto copy = std::make_shared<TerrainMaterial const>(*material);
    previous.material = copy;
    require(!revision.put(0, 0, 0, previous), "Equivalent immutable snapshots do not dirty terrain repeatedly");
    TerrainMaterial::Pixels changed; std::copy(material->pixels().begin(), material->pixels().end(), changed.begin());
    std::swap(changed[0], changed[15]);
    previous.material = std::make_shared<TerrainMaterial const>(changed);
    require(revision.put(0, 0, 0, previous), "Texture-only changes invalidate the raster even when average colour stays the same");
}
void storageTests(Directory const& dir) {
    auto material = quadrants();
    TileRecord record{{0, -1, 2}, {}};
    record.tile.cells.fill(ground(material));
    record.tile.cells[3].rotation = 3;
    record.tile.cells[3].tint = rgba(80, 150, 100);
    auto file = dir.path / "snapshot.wfmap";
    writeMap(file, "test", {record});
    auto loaded = readMap(file, "test", 1);
    require(loaded[0].tile.cells == record.tile.cells, "v7 round-trips material pixels, tint, rotation, state ID and light");
    require(loaded[0].tile.cells[0].material == loaded[0].tile.cells[3].material, "Cells share their palette material after reload");
    require(std::filesystem::file_size(file) < 12 * 1024, "A uniform chunk stores one texture, not 256 copies");
    TerrainMaterialPool pool;
    auto first = readMap(file, "test", 1, &pool), second = readMap(file, "test", 1, &pool);
    require(first[0].tile.cells[0].material == second[0].tile.cells[0].material, "Resident history tiles share identical materials");
    MapArchive history(dir.path / "pending.wfmap", "test", 1);
    history.update({record}); history.flush();
    auto pending = record;
    pending.tile.cells[0].material.reset();
    pending.tile.cells[0].blockLight = 14;
    history.update({pending});
    require(history.get(0, -16, 32).material && history.get(0, -16, 32).blockLight == 14
        && !pending.tile.cells[0].material, "History merges preserve pending material without mutating the background batch");
    ++pending.tile.cells[0].surfaceId;
    history.update({pending});
    require(!history.get(0, -16, 32).material, "A changed state also drops stale history detail");
    MapCache warm;
    auto waiting = record.tile.cells[0]; waiting.material.reset();
    warm.put(0, -16, 32, waiting); warm.mergeHistory({record});
    require(warm.get(0, -16, 32).material == material, "Warm start fills pending detail for an unchanged sampled state");
    auto corrupt = [&](std::streamoff offset, std::uint32_t value) {
        writeMap(file, "test", {record});
        std::fstream stream(file, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekp(offset);
        for (int n = 0; n < 4; ++n) stream.put(char(value >> (n * 8)));
        stream.close();
        rejects([&] { readMap(file, "test", 1); }, "Malformed material palette/reference is rejected");
    };
    constexpr std::streamoff palette = 8 + 4 + 4 + 4 + 24 + 256 * 8 + 256 * 4;
    corrupt(palette, 257);
    corrupt(palette + 4 + 256 * 4, 2); // Palette has only one entry.
    corrupt(palette + 4 + 256 * 4, 1 | (4u << 16));
    corrupt(palette + 4 + 256 * 4 + 4, 0); // Invalid tint alpha.
    writeMap(file, "test", {record});
    std::filesystem::resize_file(file, std::uintmax_t(palette + 12));
    rejects([&] { readMap(file, "test", 1); }, "Truncated texture bytes are rejected");
    auto legacy = dir.path / "v6.wfmap";
    {
        std::ofstream out(legacy, std::ios::binary); out << "WAYMAP06";
        auto word = [&](std::uint32_t value) { for (int n = 0; n < 4; ++n) out.put(char(value >> (n * 8))); };
        word(4); out << "test"; word(1); word(0); word(0); word(0); word(surfaceSlice); word(1); word(0);
        for (int i = 0; i < 256; ++i) { word(rgba(100, 120, 80)); word(64); }
        for (int i = 0; i < 256; ++i) word(15);
        word(0); for (int i = 0; i < 256; ++i) word(0);
    }
    auto old = readMap(legacy, "test", 1);
    require(!old[0].tile.cells[0].material && old[0].tile.cells[0].skyLight == 15, "v6 remains readable without inventing texture detail");
    writeMap(file, "test", old);
    require(readMap(file, "test", 1)[0].tile.cells == old[0].tile.cells, "Saving legacy history as v7 preserves all old terrain");
}
void preview() {
    // Deliberately synthetic brick/wood/stone patterns to verify the actual
    // rasterizer, not a claim about a particular installed resource pack.
    std::array<TerrainMaterialPtr, 3> materials;
    for (int kind = 0; kind < 3; ++kind) {
        TerrainMaterial::Pixels pixels;
        for (int z = 0; z < 16; ++z) for (int x = 0; x < 16; ++x) {
            int grain = (x * 7 + z * 13 + (x ^ z) * 3) % 17;
            bool seam = kind == 0 ? (z % 8 == 0 || (x + (z / 8) * 8) % 16 == 0) : kind == 1 ? z % 4 == 0 : false;
            pixels[z * 16 + x] = kind == 0 ? (seam ? rgba(110, 104, 94) : rgba(154 + grain, 76 + grain, 53 + grain))
                : kind == 1 ? (seam ? rgba(100, 70, 40) : rgba(167 + grain, 129 + grain, 73 + grain))
                : rgba(112 + grain, 117 + grain, 121 + grain);
        }
        materials[kind] = std::make_shared<TerrainMaterial const>(pixels);
    }
    MapCache flat, textured;
    for (int z = 0; z < 8; ++z) for (int x = 0; x < 12; ++x) {
        auto cell = ground(materials[x / 4]); cell.rotation = z >= 4 ? 1 : 0;
        textured.put(0, x, z, cell); cell.material.reset(); flat.put(0, x, z, cell);
    }
    MapView view{6, 4, 1.0 / 16, 192, 128};
    auto a = flat.rasterize(0, view), b = textured.rasterize(0, view);
    std::ofstream out("build/tests/terrain-material-comparison.ppm", std::ios::binary);
    out << "P6\n384 128\n255\n";
    for (int z = 0; z < 128; ++z) for (auto const* pixels : {&a, &b}) for (int x = 0; x < 192; ++x) {
        auto c = (*pixels)[z * 192 + x]; char rgb[]{char(c), char(c >> 8), char(c >> 16)}; out.write(rgb, 3);
    }
    require(bool(out), "Synthetic material preview saved");
}
}
int main(int argc, char** argv) {
    try {
        Directory dir;
        samplingTests(); rasterTests(dir); storageTests(dir);
        if (argc > 1 && std::string_view(argv[1]) == "--preview") preview();
        std::cout << checks << " checks passed: material texels, mips, tint, rotation, history, corruption and legacy formats.\n";
    } catch (std::exception const& e) { std::cerr << "FAILED after " << checks << " checks: " << e.what() << '\n'; return 1; }
}
