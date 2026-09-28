#include "wayfinder/MapArchive.h"
#include "wayfinder/MapMenu.h"
#include "wayfinder/SampleSchedule.h"
#include "wayfinder/TerrainColumn.h"
#include <bit>
#include <chrono>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace wayfinder;
namespace {
int  checks{};
void require(bool condition, char const* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template <class F>
void rejects(F&& f, char const* message) {
    bool threw = false;
    try {
        f();
    } catch (std::exception const&) {
        threw = true;
    }
    require(threw, message);
}
struct TestDirectory {
    std::filesystem::path path =
        std::filesystem::path("build/tests")
        / ("terrain-regression-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TestDirectory() { std::filesystem::create_directories(path); }
    ~TestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
void selectionTests() {
    require(!terrainLayer(0, 65, 48, "auto", {}).underground(), "Overworld starts on the surface above the threshold");
    auto cave = terrainLayer(0, 20, 48, "auto", {});
    require(cave == MapLayer(0, 2) && cave.referenceY() == 20, "Deep Overworld uses a height slice");
    require(!terrainLayer(0, 49, 48, "auto", {}).underground(), "Above the configured Y stays on the surface");
    require(terrainLayer(0, 48, 48, "auto", {}).underground(), "The exact configured Y enters cave mode");
    require(terrainLayer(0, 47, 48, "auto", {}).underground(), "Below the configured Y enters cave mode");
    require(terrainLayer(0, 51, 48, "auto", cave).underground(), "Return margin prevents threshold flicker");
    require(
        !terrainLayer(0, 52, 48, "auto", cave).underground(),
        "Four blocks above the threshold restores surface mode"
    );
    require(!terrainLayer(0, 20, 0, "auto", {}).underground(), "A custom lower threshold delays cave mode");
    require(terrainLayer(0, -10, -10, "auto", {}).slice == -2, "Negative switch heights are supported");
    require(!terrainLayer(0, 40, 32, "auto", cave).underground(), "Lowering the threshold updates an active cave map");
    require(terrainLayer(0, 40, 48, "auto", {}).underground(), "Raising the threshold activates caves immediately");
    require(
        !terrainLayer(0, 50, 48, "auto", {1, 2}).underground(),
        "A dimension change does not inherit another dimension's return margin"
    );
    require(
        terrainLayer(1, 60, -64, "auto", {}) == MapLayer(1, 7),
        "Nether caves are independent of the Overworld threshold"
    );
    require(
        !terrainLayer(2, 20, 48, "auto", cave).underground(),
        "End islands stay on the surface below the threshold"
    );
    require(!terrainLayer(0, 20, 48, "surface", cave).underground(), "Manual surface mode overrides the threshold");
    require(!terrainLayer(1, 60, 48, "surface", cave).underground(), "Manual surface mode is available in the Nether");
    require(terrainLayer(0, 80, 48, "cave", {}).underground(), "Manual cave mode works above the threshold");
    require(
        terrainLayer(2, -1, 48, "cave", {}) == MapLayer(2, -1),
        "Manual caves and negative slices work in every dimension"
    );
    require(
        terrainLayer(0, -8, 48, "cave", {}).slice == -1 && terrainLayer(0, -9, 48, "cave", {}).slice == -2,
        "Slice boundaries use floor division"
    );
}
void columnTests() {
    constexpr int                                       minY = -64, maxY = 160;
    std::array<std::optional<ColumnBlock>, maxY - minY> blocks;
    auto                                                solid = ColumnBlock{ColumnKind::Solid, rgba(110, 110, 110)};
    blocks.fill(solid);
    auto read = [&](int y) {
        require(y >= minY && y < maxY, "Sampling remains within build limits");
        return blocks[y - minY];
    };
    auto fill = [&](int low, int high, ColumnBlock block) {
        for (int y = low; y <= high; ++y) blocks[y - minY] = block;
    };
    fill(20, 23, {});
    auto floor = caveColumn(20, minY, maxY, read);
    require(floor && floor->floor() && floor->height == 19, "Caves show their floor, not the roof");
    floor = caveColumn(24, minY, maxY, read);
    require(floor && floor->height == 19, "Low corridors near slice edges remain visible");
    auto wall = caveColumn(40, minY, maxY, read);
    require(
        wall && wall->known() && wall->flags == MapCell::wall && !wall->floor(),
        "Solid columns remain walls, not teleport floors"
    );
    require(shadedColor(*wall, *floor, {}) == wall->color, "Walls have stable contrast without false cliff shading");
    fill(36, 39, {});
    auto upper = caveColumn(36, minY, maxY, read);
    require(upper && upper->height == 35 && floor->height == 19, "Stacked caves resolve their own floors");
    fill(-22, -17, {});
    auto belowZero = caveColumn(-20, minY, maxY, read);
    require(belowZero && belowZero->height == -23, "Below-zero caves retain signed floor heights");
    blocks[20 - minY].reset();
    require(!caveColumn(20, minY, maxY, read), "Missing reference data remains fog");
    blocks[20 - minY] = ColumnBlock{};
    blocks[19 - minY].reset();
    require(!caveColumn(20, minY, maxY, read), "Missing cave floor does not expose a lower layer");
    blocks[19 - minY] = solid;
    fill(30, 34, {ColumnKind::Water, rgba(40, 100, 210)});
    fill(35, 40, {});
    auto water = caveColumn(36, minY, maxY, read);
    require(water && water->height == 34 && water->depth == 5, "Underground water keeps its surface and depth");
    water = caveColumn(32, minY, maxY, read);
    require(water && water->height == 34 && water->depth == 5, "Underwater slices resolve the real water surface");
    blocks[32 - minY] = ColumnBlock{ColumnKind::Water, rgba(40, 100, 210), true};
    water             = caveColumn(36, minY, maxY, read);
    require(water && water->depth == 3, "Waterlogged blocks stop the depth scan");
    fill(50, 55, {});
    blocks[49 - minY] = ColumnBlock{ColumnKind::Lava, rgba(255, 100, 18)};
    auto lava         = caveColumn(52, minY, maxY, read);
    require(
        lava && lava->height == 49 && lava->color == rgba(255, 100, 18) && lava->depth == 0,
        "Nether lava remains visible without water-depth tint"
    );
    fill(45, 49, {ColumnKind::Lava, rgba(255, 100, 18)});
    lava = caveColumn(46, minY, maxY, read);
    require(lava && lava->height == 49, "Submerged Nether slices resolve the lava surface");
    blocks[48 - minY].reset();
    require(!caveColumn(46, minY, maxY, read), "Missing liquid surface data remains unknown");
    blocks[48 - minY] = ColumnBlock{ColumnKind::Lava, rgba(255, 100, 18)};
    fill(60, 100, {});
    blocks[59 - minY] = ColumnBlock{ColumnKind::Solid, rgba(220, 220, 150)};
    auto end          = columnFloor(90, minY, read);
    require(end && end->height == 59 && end->floor(), "End islands render their visible surface");
    blocks.fill(ColumnBlock{});
    auto empty = columnFloor(-1, minY, read);
    require(
        empty && empty->known() && empty->flags == MapCell::voidSpace && !empty->floor(),
        "Explored void is distinct from fog and cannot teleport"
    );
    require(!columnFloor(100, minY, read), "Bounded scans do not invent void before reaching the bottom");
    require(!caveColumn(0, 1, 1, read), "Invalid build limits are rejected");
    require(caveColumn(200, minY, maxY, read) == std::nullopt, "Above-roof sampling stays bounded");
}
void storageTests(TestDirectory const& dir) {
    std::array<MapLayer, 5> layers{
        {0, {0, -3}, {0, 2}, {1, 7}, 2}
    };
    MapCache cache(64);
    for (std::size_t i = 0; i < layers.size(); ++i) {
        for (int z = -1; z <= 16; ++z)
            for (int x = -1; x <= 16; ++x)
                cache.put(
                    layers[i],
                    x,
                    z,
                    {rgba(60 + unsigned(i) * 20, 110, 80),
                     static_cast<std::int16_t>(-24 + int(i) * 16 + floorDiv(x, 3))}
                );
        cache.put(layers[i], 0, 0, emptyTerrain(20, true));
        cache.put(layers[i], 1, 0, emptyTerrain(-64, false));
    }
    auto records = cache.snapshot();
    auto file    = dir.path / "layers.wfmap";
    writeMap(file, "layers", records);
    auto loaded = readMap(file, "layers", 100);
    require(loaded.size() == records.size(), "All dimensions and cave layers persist");
    MapCache restored(64);
    restored.restore(loaded);
    MapArchive archive(dir.path / "archive.wfmap", "layers", 1);
    archive.update(records);
    archive.flush();
    MapArchive reopened(dir.path / "archive.wfmap", "layers", 1);
    require(reopened.bounds().size() == layers.size(), "History bounds are isolated by dimension and cave layer");
    for (auto layer : layers) {
        require(restored.get(layer, -1, -1) == cache.get(layer, -1, -1), "Signed coordinates and heights round-trip");
        require(reopened.get(layer, 0, 0).flags == MapCell::wall, "Walls survive archive eviction and reload");
        require(reopened.get(layer, 1, 0).flags == MapCell::voidSpace, "Void survives archive eviction and reload");
        for (double scale : {0.5, 2.0, 8.0, 16.0}) {
            MapView view{8, 8, scale, 32, 32};
            require(
                cache.rasterize(layer, view) == reopened.rasterize(layer, view),
                "Live/history cave shading agrees across edges and zoom levels"
            );
        }
        MapView   distant{8, 8, 32, 4, 4};
        MapRaster overview(distant);
        for (auto const& record : records)
            if (record.key.layer() == layer)
                overview.add(record.key.x * 16.0, record.key.z * 16.0, 16, tileOverview(record.tile));
        require(
            overview.finish() == reopened.rasterize(layer, distant),
            "Far-zoom summaries include only their own cave layer"
        );
    }
    require(!reopened.get({0, 3}, 5, 5).known(), "An unexplored cave cannot borrow another layer");
    require(
        reopened.get(0, 5, 5) != reopened.get({0, 2}, 5, 5),
        "Surface and cave heights do not overwrite each other"
    );
    require(
        std::filesystem::exists(dir.path / "archive.wfmap.tiles/0_0_0.wfmap")
            && std::filesystem::exists(dir.path / "archive.wfmap.tiles/0_0_0_c-3.wfmap"),
        "Legacy surface filenames and signed cave filenames coexist"
    );
    auto bad      = records.front();
    bad.key.slice = 4096;
    writeMap(dir.path / "bad-slice.wfmap", "layers", {bad});
    rejects([&] { readMap(dir.path / "bad-slice.wfmap", "layers", 1); }, "Invalid cave slice is rejected");
    bad.key.slice           = surfaceSlice;
    bad.tile.cells[0].flags = 4;
    writeMap(dir.path / "bad-flags.wfmap", "layers", {bad});
    rejects([&] { readMap(dir.path / "bad-flags.wfmap", "layers", 1); }, "Unknown terrain flags are rejected");
    MapCache small(1);
    small.put({0, 2}, -1, -1, {rgba(100, 110, 120), 19});
    small.put({1, 7}, -1, -1, {rgba(150, 80, 70), 59});
    auto dirty = small.takeChanges();
    require(
        dirty.size() == 2 && dirty[0].key.layer() == MapLayer(0, 2),
        "Evicted cave changes keep their layer for saving"
    );
    SampleSchedule schedule;
    schedule.recenter({0, 0, 0, 2}, 1, [](auto) { return false; });
    schedule.dirty({0, 0, 0, 2});
    schedule.recenter({0, 0, 0, 3}, 1, [](auto) { return false; });
    for (int n = 0; n < 20; ++n) {
        auto next = schedule.next();
        require(next && next->slice == 3, "Slice changes discard stale scanning and dirty tasks");
    }
}
// Hand-written fixtures exercise the actual historical v1/v2/v3 wire formats.
void legacyTests(TestDirectory const& dir) {
    for (int version = 1; version <= 3; ++version) {
        auto path = dir.path / ("legacy-v" + std::to_string(version) + ".wfmap");
        {
            std::ofstream out(path, std::ios::binary);
            out << "WAYMAP0" << version;
            auto word = [&](std::uint32_t n) {
                for (int b = 0; b < 4; ++b) out.put(char((n >> (8 * b)) & 255));
            };
            word(6);
            out << "legacy";
            word(1);
            word(0);
            word(static_cast<std::uint32_t>(-1));
            word(2);
            if (version == 3) {
                word(42);
                word(1);
            }
            for (int i = 0; i < 256; ++i) {
                word(rgba(30, 80, 170));
                word(version == 1 ? static_cast<std::uint32_t>(-20) : 0xffecu | (5u << 16));
            }
        }
        auto loaded = readMap(path, "legacy", 1);
        require(loaded.size() == 1 && loaded[0].key.layer() == MapLayer(0), "Old formats default to the surface layer");
        require(
            loaded[0].tile.cells[0].height == -20 && loaded[0].tile.cells[0].flags == 0
                && loaded[0].tile.cells[0].depth == (version == 1 ? 0 : 5),
            "Old signed heights and water depth remain intact"
        );
        require(
            loaded[0].tile.lastTouched == (version == 3 ? (1ull << 32) + 42 : 1),
            "Old access order survives migration"
        );
        MapArchive archive(path, "legacy", 1);
        require(
            archive.get(0, -16, 32).height == -20 && std::filesystem::exists(path),
            "Legacy migration preserves terrain and original file"
        );
        require(!archive.get({0, -3}, -16, 32).known(), "Legacy surface history never becomes cave history");
    }
}
void settingsTests(TestDirectory const& dir) {
    Settings   settings;
    Navigation navigation;
    MapMenu    menu;
    UiButton   button{};
    button.action = UiAction::TerrainMode;
    for (auto expected : {"surface", "cave", "auto"}) {
        require(
            menu.action(button, settings, navigation) == 2 && settings.terrainMode == expected,
            "Terrain mode cycles and requests an immediate save"
        );
    }
    settings.terrainMode = "cave";
    require(settings.caveSwitchY == 48, "The default cave switch height is Y=48");
    button.action = UiAction::CaveHeightDown;
    require(
        menu.action(button, settings, navigation) == 2 && settings.caveSwitchY == 47,
        "The height minus control adjusts one block and requests a save"
    );
    button.action = UiAction::CaveHeightUp;
    require(
        menu.action(button, settings, navigation) == 2 && settings.caveSwitchY == 48,
        "The height plus control adjusts one block and requests a save"
    );
    settings.caveSwitchY = -24;
    saveSettings(dir.path / "settings", settings);
    require(loadSettings(dir.path / "settings").terrainMode == "cave", "Terrain mode persists");
    require(loadSettings(dir.path / "settings").caveSwitchY == -24, "A custom negative switch height persists");
    auto json = nlohmann::json::parse(std::ifstream(dir.path / "settings/config.json"));
    json.erase("terrainMode");
    json.erase("caveSwitchY");
    atomicText(dir.path / "settings/config.json", json.dump());
    require(loadSettings(dir.path / "settings").terrainMode == "auto", "Old configs enable automatic terrain mode");
    require(loadSettings(dir.path / "settings").caveSwitchY == 48, "Old configs use the default switch height");
    settings.caveSwitchY = -10000;
    normalizeSettings(settings);
    require(settings.caveSwitchY == -64, "The switch height clamps to the supported lower limit");
    button.action = UiAction::CaveHeightDown;
    require(
        menu.action(button, settings, navigation) == 0 && settings.caveSwitchY == -64,
        "The lower UI limit does not trigger redundant saves"
    );
    settings.caveSwitchY = 10000;
    normalizeSettings(settings);
    require(settings.caveSwitchY == 320, "The switch height clamps to the supported upper limit");
    button.action = UiAction::CaveHeightUp;
    require(
        menu.action(button, settings, navigation) == 0 && settings.caveSwitchY == 320,
        "The upper UI limit does not trigger redundant saves"
    );
    resetMapPreferences(settings);
    require(settings.caveSwitchY == 48, "Resetting preferences restores the default switch height");
    settings.terrainMode = "invalid";
    normalizeSettings(settings);
    require(settings.terrainMode == "auto", "Unknown terrain modes fall back to automatic");
    menu.open(MapMenu::Page::Settings);
    menu.settingsTab = 1;
    auto frame       = menu.build(420, 280, settings, navigation);
    require(
        std::any_of(
            frame.buttons.begin(),
            frame.buttons.end(),
            [](auto const& b) { return b.action == UiAction::TerrainMode; }
        ),
        "Terrain mode is reachable in the overlay settings"
    );
    for (auto action : {UiAction::CaveHeightDown, UiAction::CaveHeightUp})
        require(
            std::any_of(frame.buttons.begin(), frame.buttons.end(), [&](auto const& b) { return b.action == action; }),
            "Both height controls are reachable in overlay settings"
        );
}
} // namespace
int main() {
    try {
        TestDirectory dir;
        selectionTests();
        columnTests();
        storageTests(dir);
        legacyTests(dir);
        settingsTests(dir);
        std::cout << checks
                  << " checks passed: terrain selection, cave/Nether/End columns, layers, archives and legacy "
                     "compatibility.\n";
    } catch (std::exception const& error) {
        std::cerr << "FAILED after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
