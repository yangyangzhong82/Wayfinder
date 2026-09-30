#include "wayfinder/MapArchive.h"
#include "wayfinder/MapMenu.h"
#include "wayfinder/MapInspection.h"
#include <fstream>
#include <iostream>
#include <chrono>

using namespace wayfinder;
namespace {
int checks{};
void require(bool value, char const* message) { ++checks; if (!value) throw std::runtime_error(message); }
template <class F> void rejects(F&& fn, char const* message) {
    bool failed = false; try { fn(); } catch (std::exception const&) { failed = true; }
    require(failed, message);
}
struct Directory {
    std::filesystem::path path = std::filesystem::path("build/tests")
        / ("biome-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Directory() { std::filesystem::create_directories(path); }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
MapCell ground{rgba(80, 120, 65), 64};
void paletteTests() {
    BiomeTile tile;
    require(tile.get(0).empty() && tile.overview().empty(), "Unknown tiles have no biome");
    for (int i = 0; i < 256; ++i) tile.set(i, "custom:biome_" + std::to_string(i));
    for (int cycle = 0; cycle < 3; ++cycle)
        for (int i = 0; i < 256; ++i) tile.set(i, "custom:new_" + std::to_string(cycle * 256 + i));
    require(tile.names.size() <= 256, "Repeated biome edits keep the palette bounded");
    for (int i = 0; i < 256; ++i)
        require(tile.get(i) == "custom:new_" + std::to_string(512 + i), "Palette recycling preserves other columns");
    for (int i = 0; i < 256; ++i) tile.set(i, i < 200 ? "minecraft:plains" : "minecraft:forest");
    require(tile.overview() == "minecraft:plains", "Far overview selects the dominant sampled biome");
    require(!tile.set(0, "") && tile.get(0) == "minecraft:plains", "Unavailable samples do not erase saved biomes");
    tile.cells[0] = 0;
    require(tile.overview().empty(), "Incomplete chunks do not pretend their unknown area is explored");
}
void persistenceTests(Directory const& dir) {
    MapCache cache(2);
    cache.put({0}, -1, -1, ground, "minecraft:plains");
    auto revision = cache.revision();
    require(cache.put({0}, -1, -1, ground, "minecraft:forest") && cache.revision() > revision,
        "Biome-only changes invalidate the map image");
    cache.put({0, -3}, -1, -1, ground, "minecraft:lush_caves");
    MapArchive archive(dir.path / "regions.wfmap", "biome-world", 1);
    archive.update(cache.takeChanges()); archive.flush();
    MapArchive reopened(dir.path / "regions.wfmap", "biome-world", 1);
    require(reopened.biome({0}, -1, -1) == "minecraft:forest"
        && reopened.biome({0, -3}, -1, -1) == "minecraft:lush_caves"
        && reopened.biome({1}, -1, -1).empty(), "Persisted biomes isolate dimensions and cave heights");
    MapCache partial(1);
    partial.put(0, -2, -1, ground, "custom:orchard");
    reopened.update(partial.takeChanges()); reopened.flush();
    require(reopened.biome(0, -1, -1) == "minecraft:forest"
        && reopened.biome(0, -2, -1) == "custom:orchard", "Partial chunk updates retain other saved biome names");
    partial.put(0, -1, -1, ground); reopened.update(partial.takeChanges());
    require(reopened.biome(0, -1, -1) == "minecraft:forest", "Old terrain-only samples do not erase biome history");
    MapCache fresh(1); fresh.put(0, -1, -1, ground, "custom:fresh");
    fresh.mergeHistory(cache.snapshot());
    require(fresh.biome(0, -1, -1) == "custom:fresh", "Warm start does not overwrite new live biomes");
    auto fixture = dir.path / "v4.wfmap";
    {
        std::ofstream out(fixture, std::ios::binary); out << "WAYMAP04";
        auto word = [&](std::uint32_t value) { for (int i = 0; i < 4; ++i) out.put(char((value >> (8 * i)) & 255)); };
        word(3); out << "old"; word(1); word(0); word(0); word(0); word(surfaceSlice); word(1); word(0);
        for (int i = 0; i < 256; ++i) { word(ground.color); word(64); }
    }
    auto old = readMap(fixture, "old", 1);
    require(old.size() == 1 && old[0].tile.cells[0] == ground && old[0].tile.biomes.get(0).empty(),
        "Version 4 history remains readable without inventing biome data");
    writeMap(fixture, "old", old);
    require(readMap(fixture, "old", 1)[0].tile.cells[0] == ground, "Old terrain survives a version 5 save");
    {
        std::ofstream out(fixture, std::ios::app | std::ios::binary); out.put('x');
    }
    rejects([&] { readMap(fixture, "old", 1); }, "Trailing data is rejected in variable-sized records");
    writeMap(fixture, "old", old);
    std::filesystem::resize_file(fixture, std::filesystem::file_size(fixture) - 1);
    rejects([&] { readMap(fixture, "old", 1); }, "Truncated biome indices are rejected");
    writeMap(fixture, "old", old);
    {
        std::fstream file(fixture, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(-4, std::ios::end); file.put(1); // Empty palette, invalid index.
    }
    rejects([&] { readMap(fixture, "old", 1); }, "Out-of-range palette indices are rejected");
    rejects([&] { readMap(dir.path / "regions.wfmap.tiles/0_-1_-1.wfmap", "another-world", 1); },
        "Biome history obeys world identity checks");
}
void rasterTests(Directory const& dir, bool preview) {
    MapCache cache(128);
    for (int z = -32; z < 32; ++z) for (int x = -32; x < 32; ++x) {
        if (x >= 24 && z >= 24) continue;
        auto biome = x < 0 ? "minecraft:forest" : "minecraft:desert";
        if (x > 7 && x < 18 && z > -10 && z < 10) biome = "minecraft:plains";
        cache.put(0, x, z, ground, biome);
    }
    MapArchive archive(dir.path / "render.wfmap", "render", 1);
    archive.update(cache.takeChanges()); archive.flush();
    for (MapView view : {MapView{0, 0, 0.25, 256, 256}, MapView{-0.25, 0.75, 0.5, 144, 144},
                        MapView{0, 0, 16, 64, 64}, MapView{0, 0, 0.0625, 1024, 1024}}) {
        auto live = rasterizeBiomes(cache, 0, view), saved = archive.biomes(0, view);
        require(live.width <= 512 && live.height <= 512, "Biome raster and region analysis stay bounded");
        for (int z = 0; z < live.height; ++z) for (int x = 0; x < live.width; ++x) {
            auto name = [&](BiomeMap const& map) { auto id = map.at(x, z); return id ? map.names[id - 1] : std::string{}; };
            require(name(live) == name(saved), "Live/history biome grids agree across chunk seams and zoom levels");
        }
        auto pixels = cache.rasterize(0, view);
        auto original = pixels;
        overlayBiomes(pixels, view, live);
        require(pixels != original, "Enabling the overlay changes sampled biome regions");
        for (auto const& label : live.labels)
            require(live.atWorld(label.x, label.z) == label.biome, "Region labels stay inside their own connected biome");
        if (view.width == 256) {
            auto index = [&](double x, double z) { return int((z + 32) / 0.25) * 256 + int((x + 32) / 0.25); };
            require(pixels[index(28, 28)] == original[index(28, 28)], "Unknown terrain gets no biome tint");
            require((pixels[index(-0.125, -20)] & 255) > (pixels[index(-8, -20)] & 255),
                "Different known biomes have a bright boundary at the true chunk seam");
            require(pixels[index(23.5, 28)] == pixels[index(20.5, 28)], "Unknown frontier is not outlined as a biome boundary");
            require(live.labels.size() >= 3, "Distinct connected biomes receive region name anchors");
            if (preview) {
                std::ofstream out("build/tests/biome-regions.ppm", std::ios::binary);
                out << "P6\n256 256\n255\n";
                for (auto pixel : pixels) { out.put(char(pixel)); out.put(char(pixel >> 8)); out.put(char(pixel >> 16)); }
            }
        }
    }
    auto other = rasterizeBiomes(cache, {0, -3}, MapView{});
    require(other.labels.empty() && std::all_of(other.cells.begin(), other.cells.end(), [](auto id) { return id == 0; }),
        "Surface biome data never bleeds into a cave layer");
}
void settingsTests(Directory const& dir) {
    Settings settings; MapMenu menu; Navigation nav;
    require(!settings.showBiomeRegions, "Biome regions default off on old configurations");
    UiButton button; button.action = UiAction::BiomeRegions;
    require(menu.action(button, settings, nav) == 2 && settings.showBiomeRegions, "Biome region toggle requests saving");
    saveSettings(dir.path / "settings", settings);
    require(loadSettings(dir.path / "settings").showBiomeRegions, "Biome region setting persists");
    settings.showBiome = false;
    require(settings.showBiomeRegions, "Biome regions are independent of the player/cursor name switch");
    menu.open(MapMenu::Page::Settings); menu.settingsTab = 1;
    bool found = false;
    for (int page = 0; page < 20; ++page) {
        auto frame = menu.build(420, 280, settings, nav);
        for (auto const& item : frame.buttons) if (item.action == UiAction::BiomeRegions) found = item.selected && item.toggle;
        UiButton next; next.action = UiAction::Next; menu.action(next, settings, nav);
    }
    require(found, "Biome regions toggle is reachable in paginated settings");
    menu.action(button, settings, nav);
    require(!settings.showBiomeRegions, "Biome region switch turns off independently");
}
}
int main(int argc, char**) {
    try {
        Directory dir; paletteTests(); persistenceTests(dir); rasterTests(dir, argc > 1); settingsTests(dir);
        std::cout << checks << " checks passed: biome palettes, persistence, migration, boundaries, labels and settings.\n";
    } catch (std::exception const& ex) { std::cerr << "FAILED: " << ex.what() << '\n'; return 1; }
}
