#include "wayfinder/MapArchive.h"
#include "wayfinder/MapMarkers.h"
#include "wayfinder/MapMenu.h"
#include "wayfinder/MapTeleport.h"
#include "wayfinder/WayfinderView.h"
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>

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
    require(light(shadedColor(land(72), land(71), land(72))) < light(shadedColor(land(71), land(70), land(71))), "Eight-block contours outline elevation bands");
    require(light(shadedColor(land(-8), land(-9), land(-8))) < light(shadedColor(land(-7), land(-8), land(-7))), "Contour bands use floor division below zero");
    MapCell shallow{rgba(45, 105, 220), 62, 2}, deep = shallow; deep.depth = 40;
    require(light(shadedColor(shallow, {}, {})) > light(shadedColor(deep, {}, {})), "Water depth distinguishes shallow shores from deep channels");
    require(shadedColor(shallow, land(160), land(-64)) == shadedColor(shallow, {}, {}), "Shore cliffs do not imprint land shading onto water");
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
    for (double scale : {0.5, 1.0, 2.0, 8.0, 16.0}) {
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
    require(s.minimapBlocksPerPixel == 0.5 && !zoomMinimap(s, true), "Minimap zoom upper bound is stable");
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
} // namespace
int main() {
    try {
        TestDirectory dir;
        archiveTests(dir); warmStartTests(); waypointTests(dir); markerTests(); menuAndSettingsTests(dir);
        teleportAndContextTests(); reliefTests(dir);
        std::cout << checks << " checks passed: history, waypoints, settings, teleport menu/coordinates and terrain relief.\n";
    } catch (std::exception const& ex) {
        std::cerr << "FAILED after " << checks << " checks: " << ex.what() << '\n';
        return 1;
    }
}
