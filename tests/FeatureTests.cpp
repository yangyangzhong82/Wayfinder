#include "wayfinder/MapArchive.h"
#include "wayfinder/MapMarkers.h"
#include "wayfinder/MapMenu.h"
#include "wayfinder/MapTeleport.h"
#include "wayfinder/MapInspection.h"
#include "wayfinder/SlimeChunks.h"
#include "wayfinder/WayfinderView.h"
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <random>

using namespace wayfinder;
namespace {
int checks{};
void require(bool condition, char const* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template <class F> void rejects(F&& f, char const* message) {
    bool threw = false;
    try { f(); } catch (std::exception const&) { threw = true; }
    require(threw, message);
}
struct TestDirectory {
    std::filesystem::path path = std::filesystem::path("build/tests")
        / ("feature-regression-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TestDirectory() { std::filesystem::create_directories(path); }
    ~TestDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
TileRecord tile(int x, int z, std::uint32_t color = rgba(70, 150, 90)) {
    TileRecord result{};
    result.key = {0, x, z};
    result.tile.lastTouched = std::uint64_t(x + 10);
    result.tile.cells.fill(MapCell{color, 64});
    return result;
}
void damage(std::filesystem::path const& path) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << "corrupt tile bytes";
}
void archiveTests(TestDirectory const& dir) {
    auto path = dir.path / "history.wfmap";
    auto folder = std::filesystem::path(path.string() + ".tiles");
    {
        MapArchive archive(path, "world", 2);
        archive.update({tile(0, 0), tile(1, 0)});
        archive.flush();
    }
    damage(folder / "0_2_0.wfmap");
    auto progress = std::make_shared<ArchiveLoadProgress>();
    auto future = std::async(std::launch::async, [=] { return std::make_shared<MapArchive>(path, "world", 1, progress); });
    auto archive = future.get();
    require(progress->phase.load() == ArchiveLoadProgress::Phase::Ready, "Background load finishes");
    require(progress->total.load() == 3 && progress->completed.load() == 3, "Progress includes rejected tiles");
    require(archive->size() == 2 && archive->rejectedTiles() == 1, "One bad tile does not hide healthy history");
    require(!archive->takeWarnings().empty(), "Quarantine reports its path");
    std::filesystem::path backup;
    for (auto const& file : std::filesystem::directory_iterator(folder))
        if (file.path().filename().string().starts_with("0_2_0.wfmap.corrupt-")) backup = file.path();
    require(!backup.empty(), "Bad bytes are preserved in a separate file");
    require(std::filesystem::file_size(backup) == 18, "Isolation preserves the original content");
    require(archive->recent(3).size() == 2, "Healthy records remain readable with a one-tile cache");
    archive->update({tile(2, 0)});
    archive->flush();
    require(std::filesystem::exists(backup), "Resampling keeps the corruption backup");
    {
        MapArchive reopened(path, "world", 1);
        require(reopened.size() == 3 && reopened.rejectedTiles() == 0, "Reopen ignores quarantined files");
        damage(folder / "0_1_0.wfmap");
        auto pixels = reopened.rasterize(0, MapView{24, 8, 1, 64, 32});
        require(pixels.size() == 64 * 32 && reopened.rejectedTiles() == 1, "Late corruption isolates during rasterization");
        require(reopened.size() == 2, "Late corruption removes only the bad index entry");
    }
    {
        MapArchive missing(path, "world", 1);
        std::filesystem::remove(folder / "0_2_0.wfmap");
        missing.rasterize(0, MapView{40, 8, 1, 16, 16});
        missing.update({tile(2, 0)});
        missing.flush();
        require(std::filesystem::exists(folder / "0_2_0.wfmap"), "A missing tile can be sampled and saved again");
    }
    rejects([&] { MapArchive wrong(path, "another-world", 2); }, "World identity failure does not mix archives");
    auto legacy = dir.path / "legacy.wfmap";
    writeMap(legacy, "legacy", {tile(-1, 0)});
    MapArchive migrated(legacy, "legacy", 2);
    require(migrated.size() == 1 && std::filesystem::exists(legacy), "Legacy migration retains original snapshot");
    auto badLegacy = dir.path / "bad-legacy.wfmap";
    auto badFolder = std::filesystem::path(badLegacy.string() + ".tiles");
    writeMap(badFolder / "0_0_0.wfmap", "legacy", {tile(0, 0)});
    damage(badLegacy);
    MapArchive partial(badLegacy, "legacy", 1);
    require(partial.size() == 1 && std::filesystem::exists(badLegacy), "Bad legacy file preserves valid migrated tiles");
}
void warmStartTests() {
    MapCache cache(1);
    MapCell current{rgba(255, 100, 50), 72};
    cache.put(0, 16, 0, current); // Evicted while background loading is still running.
    cache.put(0, 0, 0, current);
    cache.mergeHistory({tile(0, 0), tile(1, 0)});
    require(cache.get(0, 0, 0) == current, "Warm start never rolls back a fresh sample");
    require(cache.get(0, 1, 0).known(), "Warm start fills previously unknown cells");
    require(cache.size() == 1, "Warm start respects cache capacity");
    auto changes = cache.takeChanges();
    require(changes.size() == 2, "Warm start preserves both dirty and evicted data");
    require(changes.front().tile.cells[0] == current && changes.back().tile.cells[0] == current, "Fresh values survive save batching");
}
Navigation sampleNavigation() {
    Navigation nav;
    Waypoint p;
    p.name = "Far HOME"; p.x = 100; p.group = "基地"; p.created = 20; nav.save(p);
    p.name = "Near home"; p.x = 1; p.created = 10; nav.save(p);
    p.name = "下界基地"; p.dimension = 1; p.x = 0; p.group = "传送门"; p.created = 30; nav.save(p);
    return nav;
}
void waypointTests(TestDirectory const& dir) {
    auto nav = sampleNavigation();
    WaypointQuery query;
    query.search = "HOME";
    auto rows = queryWaypoints(nav, query);
    require(rows.size() == 2, "Name search is ASCII case insensitive");
    query.search = "基地";
    require(queryWaypoints(nav, query).size() == 1, "Chinese name search preserves UTF-8");
    query.search.clear(); query.sort = WaypointQuery::Sort::Distance;
    rows = queryWaypoints(nav, query);
    require(rows[0]->name == "Near home" && rows.back()->dimension == 1, "Distance sorting keeps other dimensions last");
    query.group = "基地"; query.dimension = 0;
    require(queryWaypoints(nav, query).size() == 2, "Group and dimension filters combine");
    query.dimension = 1;
    require(queryWaypoints(nav, query).empty(), "Empty intersection is safe");
    auto file = dir.path / "waypoints.json";
    writeNavigation(file, "world", nav);
    require(readNavigation(file, "world").points == nav.points, "Waypoint groups survive restart");
    auto data = nlohmann::json::parse(std::ifstream(file));
    for (auto& p : data["points"]) p.erase("group");
    atomicText(file, data.dump());
    auto legacy = readNavigation(file, "world");
    require(std::all_of(legacy.points.begin(), legacy.points.end(), [](auto const& p) { return p.group.empty(); }),
            "Old waypoint JSON remains compatible");
    require(cleanGroup("  基地  ") == "基地" && cleanGroup("   ").empty(), "Group input trims spaces and supports ungrouped");
}
void markerTests() {
    Navigation nav;
    for (int i = 0; i < 16; ++i) {
        Waypoint p; p.x = i % 4 * 12 - 24; p.z = i / 4 * 12 - 24; p.name = "Marker " + std::to_string(i);
        nav.save(p);
    }
    Waypoint duplicate = nav.points.front(); duplicate.id = 0; nav.target = nav.save(duplicate);
    MapRect area{10, 20, 300, 200};
    MapView view{0, 0, 1, 300, 200};
    auto markers = layoutMarkers(nav, view, area, 0, true, true, true);
    require(markers.back().target, "Target draws above overlapping markers");
    require(hitMarker(markers, markers.back().x, markers.back().y) == nav.target, "Hit testing matches draw order");
    for (std::size_t i = 0; i < markers.size(); ++i) {
        if (!markers[i].label) continue;
        auto label = *markers[i].label;
        require(area.contains(label.x, label.y) && label.x + label.width <= area.x + area.width
            && label.y + label.height <= area.y + area.height, "Labels stay inside viewport");
        for (std::size_t j = 0; j < markers.size(); ++j) {
            require(!label.intersects({markers[j].x - 8, markers[j].y - 8, 16, 16}), "Labels avoid icons");
            if (i != j && markers[j].label) require(!label.intersects(*markers[j].label), "Dense labels do not overlap");
        }
    }
    require(layoutMarkers(nav, view, area, 0, false, false, true).empty(), "Hidden markers are not interactive");
    require(layoutMarkers(nav, view, area, 1, true, true, true).empty(), "Other-dimension markers remain hidden");
    auto target = *nav.find(nav.target); target.x = 2000; nav.save(target);
    auto only = layoutMarkers(nav, view, area, 0, false, true, false);
    require(only.size() == 1 && only.front().projection.outside, "Offscreen navigation arrow remains clickable");
}
UiButton action(UiAction value, std::uint64_t id = 0) { return {{}, {}, value, id}; }
void teleportAndContextTests() {
    Waypoint point;
    point.name = "Destination"; point.x = -1; point.z = -17; point.y = 63;
    require(teleportCommand(point, 0) == "/tp @s -0.5 64 -16.5 false",
            "Teleport centers negative coordinates and disables destination block checks for unloaded areas");
    point.x = 0; point.z = 17; point.y = -64;
    require(teleportCommand(point, 0) == "/tp @s 0.5 -63 17.5 false", "Negative surface heights remain signed");
    struct CommaPunctuation : std::numpunct<char> {
        char do_decimal_point() const override { return ','; }
    };
    auto previousLocale = std::locale();
    std::locale::global(std::locale(previousLocale, new CommaPunctuation));
    auto localizedCommand = teleportCommand(point, 0);
    std::locale::global(previousLocale);
    require(localizedCommand == "/tp @s 0.5 -63 17.5 false", "System locale cannot change command decimals");
    rejects([&] { teleportCommand(point, 1); }, "Stale destinations cannot teleport across dimensions");
    point.y.reset();
    rejects([&] { teleportCommand(point, 0); }, "Unknown terrain never substitutes the player's altitude");
    point.y = 32767;
    rejects([&] { teleportCommand(point, 0); }, "Landing height cannot overflow");
    point.y = 64; point.x = 30000000;
    rejects([&] { teleportCommand(point, 0); }, "Out-of-world destinations are rejected");
    point.x = -33;
    MapMenu menu; Settings settings; Navigation navigation;
    menu.locale.select("zh_CN", "en_US");
    menu.context(point, 419, 279);
    auto frame = menu.build(420, 280, settings, navigation);
    require(frame.modal && menu.page == MapMenu::Page::Context, "Right click opens a modal location menu");
    for (auto size : {std::array<float, 2>{420, 280}, std::array<float, 2>{112, 144}}) {
        frame = menu.build(size[0], size[1], settings, navigation);
        require(frame.panel.x >= 0 && frame.panel.y >= 0 && frame.panel.x + frame.panel.width <= size[0]
            && frame.panel.y + frame.panel.height <= size[1], "Context menu stays onscreen near edges and after resizing");
        for (auto const& button : frame.buttons)
            require(button.rect.x >= 0 && button.rect.y >= 0 && button.rect.x + button.rect.width <= size[0]
                && button.rect.y + button.rect.height <= size[1], "All context controls stay onscreen");
    }
    auto teleport = std::find_if(frame.buttons.begin(), frame.buttons.end(), [](auto const& b) { return b.action == UiAction::Teleport; });
    require(teleport != frame.buttons.end() && teleport->enabled && teleport->text == "传送", "Known height exposes a localized teleport button");
    require(frame.hit(teleport->rect.x + 1, teleport->rect.y + 1)->action == UiAction::Teleport, "Teleport hit target matches the drawn button");
    require(menu.draft.x == -33 && menu.draft.z == 17, "Menu repositioning does not move the destination");
    menu.action(action(UiAction::ContextWaypoint), settings, navigation);
    require(menu.page == MapMenu::Page::Edit && menu.draft == point, "Create waypoint preserves the exact right-clicked location");
    point.y.reset(); menu.context(point, 0, 0);
    frame = menu.build(420, 280, settings, navigation);
    teleport = std::find_if(frame.buttons.begin(), frame.buttons.end(), [](auto const& b) { return b.action == UiAction::Teleport; });
    require(!teleport->enabled && !frame.hit(teleport->rect.x + 1, teleport->rect.y + 1), "Unknown height disables both drawing and hit testing");
    require(frame.message == menu.locale.tr("Unknown height; teleport unavailable here."), "Unknown destinations explain why teleport is disabled");
    menu.contextLoading = true;
    require(menu.build(420, 280, settings, navigation).message == menu.locale.tr("Loading terrain height..."), "Historical lookup shows progress");
    menu.action(action(UiAction::Back), settings, navigation);
    require(menu.page == MapMenu::Page::Map && !menu.contextLoading && navigation.points.empty(), "Cancel clears context without creating a waypoint");
}
void detailTests(TestDirectory const& dir) {
    auto close = [](double a, double b) { return std::abs(a - b) < 0.00002; };
    for (int density : {2, 3, 4}) for (double center : {-29999983.25, -0.25, 0.0, 123.75, 29999983.25}) {
        for (double scale : {0.125, 0.5, 2.0, 16.0, 128.0}) {
            MapView visible{center, -center, scale, 128, 96};
            auto full = detailTextureView(visible, density);
            require(full.worldAt(0, 0) == visible.worldAt(0, 0)
                && full.worldAt(full.width, full.height) == visible.worldAt(visible.width, visible.height),
                "Denser terrain preserves the complete visible world rectangle");
            auto fullUv = textureUvRect(full, visible);
            require(fullUv.x == 0 && fullUv.y == 0 && fullUv.width == 1 && fullUv.height == 1,
                "Fullscreen detail uses the entire texture");
            auto texture = detailTextureView(minimapTextureView(visible), density);
            require(textureCoversView(texture, visible, density), "Detailed minimap retains its filtering border");
            for (double motion : {-0.1, 0.0, 0.1}) {
                auto moved = visible;
                moved.centerX += motion;
                moved.centerZ -= motion;
                auto uv = textureUvRect(texture, moved);
                auto origin = texture.worldAt(uv.x * texture.width, uv.y * texture.height);
                auto expected = moved.worldAt(0, 0);
                require(close((origin[0] - expected[0]) / scale, 0)
                    && close((origin[1] - expected[1]) / scale, 0), "UV crop follows sub-block motion at both world boundaries");
                require(close(uv.width * texture.width * texture.blocksPerPixel, moved.width * scale)
                    && close(uv.height * texture.height * texture.blocksPerPixel, moved.height * scale),
                    "UV crop and marker projection retain identical world extents");
            }
            visible.centerX += 100 * scale;
            require(!textureCoversView(texture, visible, density), "Teleport requires a new detailed texture");
        }
    }
    MapCache cache;
    for (int z = 0; z < 16; ++z) for (int x = 0; x < 16; ++x)
        cache.put(0, x, z, {x % 2 ? rgba(190, 170, 150) : rgba(40, 90, 50), 64});
    MapView view{8, 8, 2, 8, 8};
    auto coarse = cache.rasterize(0, view);
    auto fine = cache.rasterize(0, detailTextureView(view));
    require(coarse[0] == coarse[1] && fine[0] != fine[1],
        "One-block terrain features survive instead of being averaged into the same color");
    MapArchive archive(dir.path / "detail.wfmap", "detail", 1);
    archive.update(cache.snapshot());
    require(archive.rasterize(0, detailTextureView(view)) == fine, "Historical and live detail are identical");
    view.blocksPerPixel = 4;
    auto balanced = cache.rasterize(0, detailTextureView(view, 2));
    auto quality = cache.rasterize(0, detailTextureView(view, 4));
    require(balanced[4 * 16 + 4] == balanced[4 * 16 + 5]
        && quality[8 * 32 + 8] != quality[8 * 32 + 9],
        "Quality preserves single-block stripes at a wider zoom than Balanced");
    require(archive.rasterize(0, detailTextureView(view, 4)) == quality, "Quality detail also works for history");
    for (auto preset : {PerformancePreset::Light, PerformancePreset::Balanced, PerformancePreset::Quality}) {
        Settings settings;
        applyPerformancePreset(settings, preset);
        WayfinderView state;
        auto visible = state.layout(800, 600, settings, 0, 0);
        auto detailed = detailTextureView(minimapTextureView(visible), settings.terrainPixelScale);
        require(detailed.width <= 1088 && detailed.height <= 1088, "Minimap detail allocation remains bounded");
        state.fullscreen = true;
        visible = state.layout(800, 600, settings, 0, 0);
        detailed = detailTextureView(visible, settings.terrainPixelScale);
        require(detailed.width <= 3072 && detailed.height <= 3072, "Fullscreen detail allocation remains bounded");
        require(settings.terrainPixelScale == (preset == PerformancePreset::Quality ? 4 : 2),
            "Only Quality opts into the higher raster density");
    }
}
void zoomTests(TestDirectory const& dir) {
    MapView view{-123.25, 85.5, 2, 512, 320};
    auto anchor = view.worldAt(73, 91);
    for (int i = 0; i < 100; ++i) view.zoomAt(0.8, 73, 91);
    require(view.blocksPerPixel == 0.125, "Fullscreen can zoom four times closer than the former limit");
    auto after = view.worldAt(73, 91);
    require(std::abs(anchor[0] - after[0]) < 1e-8 && std::abs(anchor[1] - after[1]) < 1e-8,
        "Zooming to the new limit keeps the pointer's world position fixed");
    auto limited = view;
    view.zoomAt(0.8, 73, 91);
    require(view == limited, "Zooming at the limit does not move the map");
    for (int i = 0; i < 100; ++i) view.zoomAt(1.25, 73, 91);
    require(view.blocksPerPixel == 128, "Fullscreen retains its overview zoom range");
    Settings settings;
    settings.minimapBlocksPerPixel = 0.125;
    applyPerformancePreset(settings, PerformancePreset::Quality);
    saveSettings(dir.path / "quality", settings);
    auto loaded = loadSettings(dir.path / "quality");
    require(loaded.minimapBlocksPerPixel == 0.125 && loaded.terrainPixelScale == 4
        && performancePreset(loaded) == PerformancePreset::Quality, "New zoom and Quality density survive restart");
    auto legacy = nlohmann::json::parse(std::ifstream(dir.path / "quality/config.json"));
    legacy.erase("terrainPixelScale");
    atomicText(dir.path / "legacy-quality/config.json", legacy.dump());
    require(loadSettings(dir.path / "legacy-quality").terrainPixelScale == 4, "Existing Quality configs upgrade automatically");
    legacy["refreshMilliseconds"] = 90;
    atomicText(dir.path / "legacy-custom/config.json", legacy.dump());
    require(loadSettings(dir.path / "legacy-custom").terrainPixelScale == 2, "Custom legacy configs keep their old raster density");
    settings.refreshMilliseconds = 90;
    saveSettings(dir.path / "custom-detail", settings);
    require(loadSettings(dir.path / "custom-detail").terrainPixelScale == 4, "Custom tuning retains an explicit high density");
    applyPerformancePreset(settings, PerformancePreset::Balanced);
    require(settings.terrainPixelScale == 2 && settings.minimapBlocksPerPixel == 0.125,
        "Switching out of Quality restores its cost without resetting zoom");
}
void closeupTests(TestDirectory const& dir) {
    MapCache stripes;
    for (int z = -16; z < 16; ++z) for (int x = -16; x < 16; ++x)
        stripes.put(0, x, z, {localBlock(x) % 2 ? rgba(175, 155, 120) : rgba(45, 100, 65), 64});
    for (double scale : {0.03125, 0.125, 0.3, 0.5, 0.75, 1.0}) {
        MapView view{-0.13, 0.17, scale, 32, 24};
        auto pixels = stripes.rasterize(0, view);
        bool exact = true;
        for (int row = 0; row < view.height; ++row) for (int col = 0; col < view.width; ++col) {
            auto world = view.worldAt(col + 0.5, row + 0.5);
            exact = exact && pixels[row * view.width + col]
                == stripes.get(0, blockCoordinate(world[0]), blockCoordinate(world[1])).color;
        }
        require(exact, "Close-up preserves actual biome/road colors at fractional zoom and across negative chunk seams");
        MapArchive archive(dir.path / ("closeup-" + std::to_string(scale)), "closeup", 1);
        archive.update(stripes.snapshot());
        require(archive.rasterize(0, view) == pixels, "Close-up live and one-tile historical rendering agree");
    }
    MapCache overview;
    for (int offset : {-29999968, 29999968}) {
        MapCache boundary;
        for (int z = 0; z < 16; ++z) for (int x = 0; x < 16; ++x)
            boundary.put(0, offset + x, offset + z, {x % 2 ? rgba(175, 155, 120) : rgba(45, 100, 65), 64});
        MapView view{offset + 8.13, offset + 8.17, 0.3, 32, 24};
        auto image = boundary.rasterize(0, view);
        bool exact = true;
        for (int row = 0; row < view.height; ++row) for (int col = 0; col < view.width; ++col) {
            auto world = view.worldAt(col + 0.5, row + 0.5);
            exact = exact && image[row * view.width + col]
                == boundary.get(0, blockCoordinate(world[0]), blockCoordinate(world[1])).color;
        }
        require(exact, "Near-world-limit close-ups retain block ownership without gaps");
    }
    overview.put(0, 0, 0, {rgba(40, 100, 60), 64});
    overview.put(0, 1, 0, {rgba(180, 140, 120), 64});
    require(overview.rasterize(0, MapView{1, 1, 2, 1, 1})[0] == rgba(110, 120, 90),
        "Overview retains area averaging and ignores unknown cells");
    MapView closeup{1, 1, 0.125, 16, 16};
    MapCache ledge;
    for (int z = 0; z < 2; ++z) for (int x = 0; x < 2; ++x)
        ledge.put(0, x, z, {rgba(85, 145, 70), std::int16_t(64 + z)});
    auto pixels = ledge.rasterize(0, closeup);
    auto red = [](std::uint32_t c) { return c & 255u; };
    require(red(pixels[8 * 16 + 4]) < red(pixels[12 * 16 + 4]), "A real one-block ledge has a narrow shaded edge");
    require(pixels[9 * 16 + 4] == pixels[12 * 16 + 4], "Ledge detail does not darken the whole block");
    MapCache shore;
    for (int z = 0; z < 2; ++z) for (int x = 0; x < 2; ++x)
        shore.put(0, x, z, {z ? rgba(55, 105, 180) : rgba(160, 150, 100), 64, std::uint8_t(z ? 4 : 0)});
    pixels = shore.rasterize(0, closeup);
    require(red(pixels[8 * 16 + 4]) < red(pixels[12 * 16 + 4]), "Water outlines the actual shoreline");
    require(pixels[12 * 16 + 8] == pixels[12 * 16 + 12], "Adjacent water blocks do not receive artificial grid lines");
    MapCache isolated;
    isolated.put(0, 1, 1, {rgba(85, 145, 70), 64});
    pixels = isolated.rasterize(0, closeup);
    require(pixels[8 * 16 + 8] == pixels[12 * 16 + 12], "Unknown neighbors never create relief edges");
    require(pixels[0] != pixels[8 * 16 + 8], "Unexplored close-up stays fogged");
}

void exportCloseupPreview() {
    MapCache cache;
    for (int z = 0; z < 24; ++z) for (int x = 0; x < 32; ++x) {
        bool water = x >= 22 + (z / 4) % 3;
        auto color = water ? rgba(55, 110, 170) : z >= 11 && z <= 12 ? rgba(165, 145, 105) : rgba(85, 140, 65);
        int height = x >= 9 && x <= 17 && z >= 3 && z <= 8 ? 68 : 64;
        cache.put(0, x, z, {color, std::int16_t(height), std::uint8_t(water ? 4 : 0)});
    }
    MapView view{16.08, 12.06, 0.125, 256, 192};
    MapRaster before(view);
    for (int z = 0; z < 24; ++z) for (int x = 0; x < 32; ++x) {
        ColorAggregate color;
        color.add(shadedColor(cache.get(0, x, z), cache.get(0, x, z - 1), cache.get(0, x - 1, z)));
        before.add(x, z, 1, color);
    }
    auto oldPixels = before.finish(), newPixels = cache.rasterize(0, view);
    std::ofstream out("build/tests/terrain-closeup-comparison.ppm", std::ios::binary);
    if (!out) throw std::runtime_error("Could not create close-up preview");
    out << "P6\n" << view.width * 2 << ' ' << view.height << "\n255\n";
    for (int row = 0; row < view.height; ++row) for (auto const* pixels : {&oldPixels, &newPixels})
        for (int col = 0; col < view.width; ++col) {
            auto c = (*pixels)[row * view.width + col];
            char rgb[]{char(c & 255), char((c >> 8) & 255), char((c >> 16) & 255)};
            out.write(rgb, 3);
        }
}
// Synthetic terrain rendered through the production rasterizer (not an in-game screenshot).
void exportTerrainStylePreview() {
    MapCache cache;
    for (int z = -32; z < 96; ++z) for (int x = -32; x < 128; ++x) {
        double hill = std::max(0.0, 1.0 - std::hypot((x - 28) / 30.0, (z - 24) / 26.0));
        int height = 64 + int(hill * 32);
        int bank = 76 + int(5 * std::sin(z * 0.12));
        bool water = x > bank;
        auto color = rgba(100, 150, 80);
        if (hill > 0.68) color = rgba(140, 140, 135);
        if (x >= bank - 2) color = rgba(205, 194, 142);
        if (z >= 46 && z < 49 && x < bank) color = rgba(168, 143, 99);
        if (x >= 10 && x < 25 && z >= 54 && z < 66) { color = rgba(165, 98, 70); height = 70; }
        if (x >= 40 && x < 67 && z >= 53 && z < 70) {
            int dx = localBlock(x - 40) % 8 - 3, dz = localBlock(z - 53) % 8 - 3;
            if (dx * dx + dz * dz < 12) { color = rgba(63, 105, 45); height = 68 + (3 - std::abs(dx)); }
        }
        int depth = water ? std::min(40, x - bank) : 0;
        if (water) { color = rgba(55, 110, 180); height = 63; }
        cache.put(0, x, z, {color, std::int16_t(height), std::uint8_t(depth), 0, 15, 0});
    }
    MapView view{48.17, 36.13, 0.375, 256, 192};
    auto pixels = cache.rasterize(0, view);
    std::ofstream out("build/tests/terrain-style.ppm", std::ios::binary);
    if (!out) throw std::runtime_error("Could not create terrain style preview");
    out << "P6\n" << view.width << ' ' << view.height << "\n255\n";
    for (auto c : pixels) {
        char rgb[]{char(c & 255), char((c >> 8) & 255), char((c >> 16) & 255)};
        out.write(rgb, 3);
    }
}
void reliefTests(TestDirectory const& dir) {
    auto land = [](int height) { return MapCell{rgba(100, 150, 80), static_cast<std::int16_t>(height)}; };
    auto light = [](std::uint32_t color) { return (color & 255u) + ((color >> 8) & 255u) + ((color >> 16) & 255u); };
    auto center = land(71);
    auto flat = light(shadedColor(center, center, center));
    require(light(shadedColor(center, land(70), center)) > flat, "North-facing rises catch light");
    require(light(shadedColor(center, center, land(70))) > flat, "East-west height changes are visible");
    require(light(shadedColor(center, land(65), center)) > light(shadedColor(center, land(70), center)), "Larger height differences produce stronger relief");
    require(light(shadedColor(center, land(80), center)) < light(shadedColor(center, land(72), center)), "Cliffs are darker than gentle downward slopes");
    require(shadedColor(center, {}, {}) == shadedColor(center, center, center), "Unexplored neighbors do not produce artificial cliffs");
    require(shadedColor({}, center, center) == 0, "Unknown terrain stays unknown");
    require(light(shadedColor(land(160), land(160), land(160))) > light(shadedColor(land(64), land(64), land(64))), "Broad plateaus remain distinguishable without a nearby slope");
    for (int height : {-16, -8, 0, 64, 72, 80, 160}) {
        auto before = light(shadedColor(land(height - 1), land(height - 2), land(height - 1)));
        auto after = light(shadedColor(land(height), land(height - 1), land(height)));
        require(std::abs(int(after) - int(before)) <= 3, "Continuous slopes have no artificial eight-block contour bands");
    }
    require(light(shadedColor(center, land(-64), land(-64))) <= flat * 1.24,
        "Tall cliffs and treetops retain material colour instead of blown-out highlights");
    require(light(shadedColor(center, land(320), land(320))) >= flat * 0.70,
        "Cliff shadows retain material detail");
    MapCell shallow{rgba(45, 105, 220), 62, 2}, deep = shallow; deep.depth = 40;
    require(light(shadedColor(shallow, {}, {})) > light(shadedColor(deep, {}, {})), "Water depth distinguishes shallow shores from deep channels");
    require(shadedColor(shallow, land(160), land(-64)) == shadedColor(shallow, {}, {}), "Shore cliffs do not imprint land shading onto water");
    require(shadedColor(center, deep, deep) == shadedColor(center, {}, {}),
        "Water surfaces do not create false relief on adjacent dry land");
    MapCache cache(16);
    std::vector<TileRecord> records;
    for (int z = -1; z <= 1; ++z) for (int x = -1; x <= 1; ++x) {
        auto record = tile(x, z);
        for (int dz = 0; dz < 16; ++dz) for (int dx = 0; dx < 16; ++dx) {
            int wx = x * 16 + dx, wz = z * 16 + dz;
            auto cell = land(64 + floorDiv(wx, 4) + floorDiv(wz, 3));
            record.tile.cells[dz * 16 + dx] = cell;
            cache.put(0, wx, wz, cell);
        }
        records.push_back(record);
    }
    auto path = dir.path / "relief.wfmap";
    { MapArchive archive(path, "relief", 1); archive.update(records); archive.flush(); }
    MapArchive archive(path, "relief", 1);
    for (double scale : {0.125, 0.3, 0.5, 0.75, 1.0, 2.0, 8.0, 16.0}) {
        MapView view{8, 8, scale, 48, 48};
        require(archive.rasterize(0, view) == cache.rasterize(0, view), "Live and historical shading agree across chunk edges with a one-tile cache");
    }
    require(archive.get(0, -1, -17) == MapCell{}, "History lookup outside exploration remains unknown");
    require(archive.get(0, -1, -1) == cache.get(0, -1, -1), "History height lookup uses exact negative block coordinates");
    require(!archive.get(1, -1, -1).known(), "History height lookup respects the dimension");
    require(archive.get(0, 31, 31) == cache.get(0, 31, 31), "History height loads evicted remote tiles");
    MapArchive distant(dir.path / "plateaus.wfmap", "plateaus", 1);
    auto low = tile(0, 0), high = tile(1, 0);
    low.tile.cells.fill(land(64)); high.tile.cells.fill(land(160));
    distant.update({low, high});
    auto lowPixel = distant.rasterize(0, MapView{0, 8, 32, 1, 1})[0];
    auto highPixel = distant.rasterize(0, MapView{32, 8, 32, 1, 1})[0];
    require(light(highPixel) > light(lowPixel), "Far-zoom history summaries retain plateau height contrast");
}
void terrainShapeTests(TestDirectory const& dir) {
    auto land = [](int y) { return MapCell{rgba(100, 150, 80), std::int16_t(y)}; };
    auto cell = land(64);
    float flat = terrainShade(cell, cell, cell, cell, cell, cell, cell, cell, cell);
    require(terrainShade(cell, cell, cell, land(68), cell) > flat,
        "A slope is visible when its only higher neighbour is south");
    require(terrainShade(cell, cell, cell, cell, land(68)) > flat,
        "A slope is visible when its only higher neighbour is east");
    require(terrainShade(cell, cell, cell, cell, cell, land(60), cell, land(68), cell) > flat,
        "A flat step still receives the wider hillside's light");
    require(terrainShade(cell, cell, cell, cell, cell, land(68), cell, land(60), cell) < flat,
        "The opposite broad slope is distinguishable at the same height and material");
    require(terrainShade(cell, land(70), land(70), land(70), land(70))
        < terrainShade(cell, land(58), land(58), land(58), land(58)),
        "Hollows have subtle occlusion while ridges keep their colour");
    require(terrainShade(cell, {}, {}, {}, {}, {}, {}, {}, {}) == flat,
        "Unexplored neighbours create neither broad shadows nor occlusion");
    MapCell water{rgba(40, 90, 180), 64, 5};
    require(terrainShade(cell, water, water, water, water, water, water, water, water) == flat,
        "Water surfaces are excluded from both near and broad land slopes");
    require(terrainShade(water, land(120), land(120), land(120), land(120))
        == terrainShade(water, {}, {}, {}, {}), "Water depth shading is independent of surrounding cliffs");
    MapTile platform;
    platform.cells.fill(cell);
    platform.cells[4 * 16 + 8] = land(72);
    require(terrainShadeAt(cell, TerrainNeighborhood(platform), 8, 8) == flat,
        "A distant sheer roof does not paint a broad light halo on flat ground");
    for (int d = 1; d <= 4; ++d) platform.cells[(8 - d) * 16 + 8] = land(64 - d);
    require(terrainShadeAt(cell, TerrainNeighborhood(platform), 8, 8) > flat,
        "Continuous gentle steps retain broader hill lighting");
    platform.cells[7 * 16 + 8] = water;
    require(terrainShadeAt(cell, TerrainNeighborhood(platform), 8, 8) == flat,
        "Broad relief does not connect two land surfaces across water");
    // Exercise all halo directions at both positive and negative chunk seams,
    // with a one-tile archive that evicts the centre while gathering neighbours.
    MapCache cache(64);
    for (int z = -32; z < 32; ++z) for (int x = -32; x < 32; ++x)
        cache.put(0, x, z, land(64 + floorDiv(x, 3) - floorDiv(z, 4)));
    MapArchive archive(dir.path / "shape.wfmap", "shape", 1);
    archive.update(cache.snapshot()); archive.flush();
    for (double scale : {0.0625, 0.125, 0.375, 1.0, 3.0, 16.0, 32.0})
        for (auto center : {-16.13, -0.13, 15.87}) {
            MapView view{center, center, scale, 24, 24};
            for (auto lighting : {MapLighting{}, MapLighting{true, 11}})
                require(cache.rasterize(0, view, lighting) == archive.rasterize(0, view, lighting),
                    "Four-direction broad relief agrees in live, evicted archive and far summaries");
        }
    for (int direction = 0; direction < 4; ++direction) {
        MapCache shore;
        auto offset = TerrainNeighborhood::directions[direction];
        shore.put(0, 0, 0, water);
        shore.put(0, offset[0], offset[1], land(64));
        auto pixels = shore.rasterize(0, MapView{0.5, 0.5, 0.125, 8, 8});
        int index = direction == 0 ? 4 : direction == 1 ? 4 * 8 : direction == 2 ? 7 * 8 + 4 : 4 * 8 + 7;
        require((pixels[index] & 255u) < (pixels[4 * 8 + 4] & 255u), "Water outlines actual shores in all four directions");
    }
}
void cliffAndPeakTests(TestDirectory const& dir) {
    auto land = [](int height) { return MapCell{rgba(120, 140, 100), std::int16_t(height), 0, 0, 15, 0}; };
    auto center = land(96);
    auto flat = terrainShade(center, center, center, center, center);
    auto peak = terrainShade(center, land(92), land(92), land(92), land(92));
    auto hollow = terrainShade(center, land(100), land(100), land(100), land(100));
    require(peak > flat + 0.08f && hollow < flat - 0.08f,
        "Symmetric summits and hollows remain visible when central gradients cancel");
    require(terrainShade(center, land(92), center, land(92), center) > flat + 0.06f,
        "A narrow ridge retains shape contrast perpendicular to the light");
    auto planar = terrainShade(center, land(95), land(95), land(97), land(97));
    require(planar > flat && planar < flat * 1.22f, "Continuous hillsides keep bounded northwest lighting");
    require(terrainShade(center, {}, {}, {}, {}) == flat, "Missing neighbours do not invent summit highlights");
    for (int direction = 0; direction < 4; ++direction) {
        auto offset = TerrainNeighborhood::directions[direction];
        MapCache cache(64);
        for (int z = -32; z < 32; ++z) for (int x = -32; x < 32; ++x)
            cache.put(0, x, z, land(x * offset[0] + z * offset[1] >= 0 ? 96 : 64));
        for (auto lighting : {MapLighting{}, MapLighting{true, 11}, MapLighting{true, 15}}) {
            auto top = cache.rasterize(0, MapView{0.5, 0.5, 1, 1, 1}, lighting)[0] & 255u;
            auto foot = cache.rasterize(0, MapView{0.5 - offset[0], 0.5 - offset[1], 1, 1, 1}, lighting)[0] & 255u;
            require(top >= foot + (lighting.enabled ? 3u : 10u),
                "Every cliff orientation separates its lip from its foot, including at night");
        }
        for (double scale : {0.125, 0.375, 1.0, 2.0}) {
            // Centre the overview pixels wholly on either side of the drop.
            auto top = cache.rasterize(0, MapView{0.5 + offset[0] * (scale - 1) * 0.5,
                0.5 + offset[1] * (scale - 1) * 0.5, scale, 1, 1})[0] & 255u;
            auto foot = cache.rasterize(0, MapView{0.5 - offset[0] * (scale + 1) * 0.5,
                0.5 - offset[1] * (scale + 1) * 0.5, scale, 1, 1})[0] & 255u;
            require(top >= foot + 5u, "Cliff separation survives close-up and minimap area filtering");
        }
        auto path = dir.path / ("cliff-" + std::to_string(direction) + ".wfmap");
        { MapArchive saved(path, "cliff", 1); saved.update(cache.snapshot()); saved.flush(); }
        MapArchive history(path, "cliff", 1);
        for (double scale : {0.125, 0.375, 1.0, 2.0, 8.0, 16.0, 32.0})
            for (auto lighting : {MapLighting{}, MapLighting{true, 11}}) {
                MapView view{-0.13, -0.17, scale, 24, 24};
                auto live = cache.rasterize(0, view, lighting), archived = history.rasterize(0, view, lighting);
                if (live != archived) {
                    auto mismatch = std::mismatch(live.begin(), live.end(), archived.begin());
                    std::cerr << "Cliff mismatch: direction=" << direction << " scale=" << scale
                        << " lighting=" << lighting.enabled << " pixel=" << (mismatch.first - live.begin())
                        << " live=" << *mismatch.first << " archive=" << *mismatch.second << '\n';
                }
                require(live == archived,
                    "Cliff relief is identical in live maps and reopened history across negative chunk seams");
            }
    }
}
// Monochrome terrain makes shape improvements visible without biome colours.
void exportCliffPeakPreview() {
    MapCache cache(128);
    for (int z = -16; z < 112; ++z) for (int x = -16; x < 144; ++x) {
        double mountain = std::max(0.0, 1.0 - std::hypot((x - 32) / 29.0, (z - 30) / 26.0));
        int height = 64 + int(mountain * 48);
        int cliff = 82 + int(5 * std::sin(z * 0.1));
        if (x >= cliff && z < 68) height = 106;
        if (z >= 70 && z < 86 && x > 12 && x < 62) height = 80;
        cache.put(0, x, z, {rgba(120, 145, 100), std::int16_t(height), 0, 0, 15, 0});
    }
    MapView view{64, 48, 0.5, 256, 192};
    auto pixels = cache.rasterize(0, view);
    std::ofstream out("build/tests/terrain-cliffs-peaks.ppm", std::ios::binary);
    require(bool(out), "Cliff and summit preview can be created");
    out << "P6\n" << view.width << ' ' << view.height << "\n255\n";
    for (auto color : pixels) {
        char rgb[]{char(color & 255), char((color >> 8) & 255), char((color >> 16) & 255)};
        out.write(rgb, 3);
    }
    require(bool(out), "Cliff and summit preview saved");
}
void menuAndSettingsTests(TestDirectory const& dir) {
    Settings s;
    auto nav = sampleNavigation();
    MapMenu menu;
    menu.open(MapMenu::Page::List);
    menu.action(action(UiAction::Search), s, nav); menu.append("home"); menu.commitField();
    auto frame = menu.build(420, 280, s, nav);
    require(std::count_if(frame.buttons.begin(), frame.buttons.end(), [](auto const& b) { return b.action == UiAction::OpenWaypoint; }) == 2,
            "Search results reach the list UI");
    menu.action(action(UiAction::ClearFilters), s, nav);
    menu.action(action(UiAction::DimensionFilter), s, nav);
    require(menu.query.dimension == 0, "Dimension filter cycles from all to Overworld");
    menu.action(action(UiAction::DimensionFilter), s, nav);
    require(menu.query.dimension == 1, "Dimension filter includes saved dimensions");
    menu.action(action(UiAction::DimensionFilter), s, nav);
    require(!menu.query.dimension, "Dimension filter returns to all");
    menu.edit(nav.points.front());
    menu.action(action(UiAction::Group), s, nav); menu.append("  新基地  "); menu.commitField();
    require(menu.action(action(UiAction::Save), s, nav) == 1 && nav.points.front().group == "新基地", "Group edit persists through save action");
    require(menu.action(action(UiAction::NavigateExisting, nav.points.front().id), s, nav) == 1
        && nav.target == nav.points.front().id && menu.page == MapMenu::Page::Map, "Direct marker navigation selects and closes menu");
    for (int i = 0; i < 100; ++i) zoomMinimap(s, true);
    require(s.minimapBlocksPerPixel == 0.125 && !zoomMinimap(s, true), "Minimap zoom upper bound is stable");
    for (int i = 0; i < 100; ++i) zoomMinimap(s, false);
    require(s.minimapBlocksPerPixel == 16 && !zoomMinimap(s, false), "Minimap zoom lower bound is stable");
    s.cacheProfile = "keep-world"; s.maxCachedChunks = 1024; s.autosaveSeconds = 60;
    menu.open(MapMenu::Page::Settings);
    require(menu.action(action(UiAction::ResetSettings), s, nav) == 0 && s.minimapBlocksPerPixel == 16, "Reset needs confirmation");
    require(menu.action(action(UiAction::ResetSettings), s, nav) == 2 && s.minimapBlocksPerPixel == 2, "Confirmed reset applies defaults");
    require(s.cacheProfile == "keep-world" && s.maxCachedChunks == 1024 && s.autosaveSeconds == 60
        && nav.points.size() == 3, "Reset preserves storage controls and waypoints");
    for (auto preset : {PerformancePreset::Light, PerformancePreset::Balanced, PerformancePreset::Quality}) {
        applyPerformancePreset(s, preset); normalizeSettings(s);
        require(performancePreset(s) == preset, "All presets roundtrip through normalization");
    }
    s.columnsPerTick = 600;
    require(performancePreset(s) == PerformancePreset::Custom, "Manual performance settings show Custom");
    saveSettings(dir.path / "config", s);
    auto loaded = loadSettings(dir.path / "config");
    require(loaded.minimapZoomInKey == s.minimapZoomInKey && loaded.cacheProfile == s.cacheProfile, "New settings persist");
    s.minimapZoomInKey = s.fullMapKey;
    rejects([&] { saveSettings(dir.path / "config", s); }, "Hotkey conflict is rejected before saving");
    require(loadSettings(dir.path / "config").minimapZoomInKey != s.fullMapKey, "Rejected key does not overwrite config");
    s = loaded;
    auto oldConfig = nlohmann::json::parse(std::ifstream(dir.path / "config/config.json"));
    oldConfig.erase("minimapZoomInKey"); oldConfig.erase("minimapZoomOutKey");
    oldConfig["fullMapKey"] = 187; oldConfig["toggleMinimapKey"] = 189;
    atomicText(dir.path / "old-config/config.json", oldConfig.dump());
    auto upgraded = loadSettings(dir.path / "old-config");
    require(upgraded.fullMapKey == 187 && upgraded.toggleMinimapKey == 189
        && upgraded.minimapZoomInKey != 187 && upgraded.minimapZoomOutKey != 189, "Old +/- map bindings upgrade without a startup conflict");
    for (int tab = 0; tab < 3; ++tab) {
        menu.settingsTab = tab;
        frame = menu.build(112, 144, s, nav);
        require(menu.visibleRows >= 1 && !frame.pagination.empty(), "Small windows preserve settings pagination");
        for (auto const& b : frame.buttons) require(b.rect.x >= 0 && b.rect.y >= 0
            && b.rect.x + b.rect.width <= 112 && b.rect.y + b.rect.height <= 144, "Controls fit a small window");
    }
    std::set<std::string_view> keys;
    for (auto const& t : translationCatalog()) {
        require(keys.insert(t.key).second, "Translation keys are unique");
        require(!t.english.empty() && !t.simplifiedChinese.empty(), "Both languages cover each translation");
    }
}
void inspectionAndSlimeTests(TestDirectory const& dir) {
    MapView view{-0.5, -16.5, 0.5, 128, 64};
    MapRect area{20, 30, 256, 128};
    auto point = mapInspectionPoint(view, area, {0}, 148, 94);
    require(point && point->x == -1 && point->z == -17, "Cursor projection floors negative block coordinates");
    require(!mapInspectionPoint(view, area, {0}, 19, 94)
        && !mapInspectionPoint(view, area, {0}, 276, 94), "Cursor outside map and on excluded edge has no biome");
    CursorBiome cursor;
    cursor.select(point); cursor.identifier = "minecraft:plains";
    cursor.select(point);
    require(cursor.identifier == "minecraft:plains", "Stable hover retains sampled biome");
    cursor.select(mapInspectionPoint(view, area, {0, -2}, 148, 94));
    require(cursor.identifier.empty(), "Changing cave layer clears old cursor biome");
    cursor.identifier = "minecraft:lush_caves";
    cursor.select(mapInspectionPoint(view, area, {1, -2}, 148, 94));
    require(cursor.identifier.empty(), "Changing dimension clears old cursor biome");
    cursor.identifier = "minecraft:hell";
    cursor.select(mapInspectionPoint(view, area, {1, -2}, 152, 94));
    require(cursor.identifier.empty(), "Moving to another block never reuses previous biome");
    cursor.identifier = "minecraft:hell"; cursor.select({});
    require(!cursor.point && cursor.identifier.empty(), "Leaving map clears cursor state");
    Locale locale; locale.select("zh_CN", "en_US");
    require(locale.biome("minecraft:plains") == "平原"
        && locale.biome("custom:forest") == "custom:forest", "Cursor biome names localize with custom fallback");

    auto reference = [](int x, int z) {
        std::mt19937 random((static_cast<std::uint32_t>(x) * 0x1f1f1f1fu) ^ static_cast<std::uint32_t>(z));
        return random() % 10 == 0;
    };
    require(!isSlimeChunk(0, 0) && isSlimeChunk(0, 9), "Bedrock known slime chunk fixtures");
    for (int x : {-1874999, -1000, -17, -1, 0, 1, 16, 999, 1874999})
        for (int z = -128; z <= 128; ++z)
            require(isSlimeChunk(x, z) == reference(x, z), "Optimized slime test matches standard MT19937, including wraparound");
    require(slimeOverlayVisible({0}, view, 1) && slimeOverlayVisible({0, -8}, view, 1),
        "Slime chunks available on Overworld surface and cave layers");
    require(!slimeOverlayVisible({1}, view, 1) && !slimeOverlayVisible({2}, view, 1)
        && !slimeOverlayVisible({0}, view, 3), "Other dimensions and unreadably small chunks hide overlay");
    require(!slimeOverlayVisible({0}, MapView{0, 0, 128, 2048, 2048}, 1), "Overlay work stays bounded");
    auto base = rgba(32, 64, 96);
    for (double center : {-16.25, 0.0, 144.25}) {
        MapView sample{0.25, center, 0.75, 96, 96};
        std::vector<std::uint32_t> pixels(sample.width * sample.height, base);
        overlaySlimeChunks(pixels, sample, {0});
        for (int y = 0; y < sample.height; ++y) for (int x = 0; x < sample.width; ++x) {
            auto world = sample.worldAt(x + 0.5, y + 0.5);
            bool slime = reference(floorDiv(blockCoordinate(world[0]), 16), floorDiv(blockCoordinate(world[1]), 16));
            require((pixels[y * sample.width + x] != base) == slime,
                "Slime fill aligns with true chunk boundaries through fractional pan and negative coordinates");
        }
        auto unchanged = std::vector<std::uint32_t>(pixels.size(), base);
        overlaySlimeChunks(unchanged, sample, {1});
        require(std::all_of(unchanged.begin(), unchanged.end(), [&](auto p) { return p == base; }),
            "Nether texture is not tinted");
    }
    Settings settings; MapMenu menu; Navigation navigation;
    require(!settings.showSlimeChunks, "Slime overlay defaults off for old configs");
    require(menu.action(action(UiAction::SlimeChunks), settings, navigation) == 2 && settings.showSlimeChunks,
        "Slime menu switch requests immediate settings save");
    saveSettings(dir.path / "slime-settings", settings);
    require(loadSettings(dir.path / "slime-settings").showSlimeChunks, "Slime overlay preference persists");
    menu.open(MapMenu::Page::Settings); menu.settingsTab = 1;
    bool found = false;
    for (int page = 0; page < 20; ++page) {
        auto frame = menu.build(420, 280, settings, navigation);
        for (auto const& button : frame.buttons)
            if (button.action == UiAction::SlimeChunks) found = button.toggle && button.selected;
        menu.action(action(UiAction::Next), settings, navigation);
    }
    require(found, "Slime overlay control is reachable through settings pagination");
    menu.action(action(UiAction::SlimeChunks), settings, navigation);
    require(!settings.showSlimeChunks, "Slime overlay can be disabled again");
}
} // namespace
int main(int argc, char** argv) {
    try {
        TestDirectory dir;
        archiveTests(dir); warmStartTests(); waypointTests(dir); markerTests(); menuAndSettingsTests(dir);
        teleportAndContextTests(); reliefTests(dir); terrainShapeTests(dir); detailTests(dir); zoomTests(dir); closeupTests(dir);
        inspectionAndSlimeTests(dir); cliffAndPeakTests(dir);
        if (argc > 1 && std::string_view(argv[1]) == "--preview") {
            exportCloseupPreview();
            exportTerrainStylePreview();
            exportCliffPeakPreview();
        }
        std::cout << checks << " checks passed: history, waypoints, settings, teleport menu/coordinates and terrain relief.\n";
    } catch (std::exception const& ex) {
        std::cerr << "FAILED after " << checks << " checks: " << ex.what() << '\n';
        return 1;
    }
}
