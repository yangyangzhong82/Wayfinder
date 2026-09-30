#include "wayfinder/ExplorationTrail.h"
#include "wayfinder/MapMarkers.h"
#include "wayfinder/MapMenu.h"
#include "wayfinder/WayfinderView.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace wayfinder;
namespace {
int checks{};
void require(bool condition, char const* message) { ++checks; if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F f, char const* message) {
    bool threw{}; try { f(); } catch (std::exception const&) { threw = true; } require(threw, message);
}
struct TestDirectory {
    std::filesystem::path path = std::filesystem::path("build/tests")
        / ("exploration-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TestDirectory() { std::filesystem::create_directories(path); }
    ~TestDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
void heightTests() {
    Waypoint p; p.x = -4; p.z = -8; p.y = -30;
    require(targetDescription(p, 0, -3.5, -30, -7.5).starts_with("Arrived"), "Same floor arrives");
    auto below = targetDescription(p, 0, -3.5, 64, -7.5);
    require(below.find("Below: 94") != std::string::npos && below.find("Arrived") == std::string::npos,
        "Matching X/Z never arrives on a different floor");
    require(targetDescription(p, 0, -3.5, -50, -7.5).find("Above: 20") != std::string::npos, "Negative-height target above");
    require(!targetDescription(p, 0, -3.5, -32, -7.5).starts_with("Arrived"), "Vertical arrival boundary is strict");
    require(!targetDescription(p, 0, -1.5, -30, -7.5).starts_with("Arrived"), "Horizontal arrival boundary is strict");
    p.y.reset();
    auto unknown = targetDescription(p, 0, -3.5, 64, -7.5);
    require(unknown.find("Height unknown") != std::string::npos && unknown.find("Arrived") == std::string::npos,
        "Missing waypoint Y cannot claim arrival");
    p.dimension = 1;
    require(targetDescription(p, 0, 0, 0, 0).starts_with("Nether |"), "Cross-dimension navigation has no fictitious distance");
    Locale zh; zh.select("zh_CN", "en_US"); p.dimension = 0; p.y = 32;
    require(targetDescription(p, 0, -3.5, 0, -7.5, zh).find("上方 32") != std::string::npos, "Height text localized");
    Navigation nav; p.y = -1; auto low = nav.save(p); p.id = 0; p.y = -8; auto same = nav.save(p);
    p.id = 0; p.y = 0; auto high = nav.save(p);
    auto markers = layoutMarkers(nav, MapView{0, 0, 1, 64, 64}, {0, 0, 128, 128}, 0, true, true, false, -4);
    require(markers.size() == 3, "Other floors retain clickable geographic markers");
    for (auto const& marker : markers)
        require(marker.opacity == (marker.id == high ? 0.4f : 1.0f), "Signed cave slices set marker opacity");
    require(low != same, "Independent waypoints retained");
    nav.target = high;
    markers = layoutMarkers(nav, MapView{0, 0, 1, 64, 64}, {0, 0, 128, 128}, 0, true, true, false, -4);
    require(markers.back().id == high && markers.back().opacity == 1, "Navigation target stays bright and on top across cave floors");
}
void trailTests(TestDirectory const& dir) {
    ExplorationTrail trail;
    require(trail.observe(0, -0.2, 64, -0.2, 1, true), "First point recorded");
    require(trail.points.front().x == -1 && trail.points.front().z == -1 && trail.points.front().start, "Floor negative coordinates");
    require(!trail.observe(0, -0.1, 64, -0.1, 2, true), "Stationary ticks do not grow history");
    trail.observe(0, 2, 64, 0, 2, true);
    require(!trail.points.back().start, "Ordinary motion keeps continuity");
    trail.observe(0, 100, 64, 0, 3, true);
    require(trail.points.back().start, "Teleport breaks the polyline");
    trail.observe(0, 102, 64, 0, 20, true);
    require(trail.points.back().start, "Observation gaps break the polyline");
    trail.observe(1, 102, 64, 0, 21, true);
    require(trail.points.back().start, "Dimension changes break the polyline");
    trail.observe({1, -1}, 102, -1, 0, 22, true);
    require(trail.points.back().start, "Layer changes break the polyline");
    auto n = trail.points.size();
    trail.observe({1, -1}, 104, -1, 0, 23, false);
    require(trail.points.size() == n, "Disabled recording preserves existing trail");
    trail.observe({1, -1}, 104, -1, 0, 24, true);
    require(trail.points.back().start, "Re-enabled recording starts a fresh segment");
    trail.observe({1, -1}, 106, -1, 0, 25, true);
    writeTrail(dir.path / "trail.json", "world", trail.points);
    auto loaded = readTrail(dir.path / "trail.json", "world");
    require(loaded.points == trail.points, "Trail layers, positions, timestamps and gaps roundtrip");
    loaded.observe({1, -1}, 108, -1, 0, 26, true);
    require(loaded.points.back().start, "Rejoining never connects separate sessions");
    rejects([&] { readTrail(dir.path / "trail.json", "another-world"); }, "World identity mismatch rejected");
    auto corrupt = nlohmann::json::parse(std::ifstream(dir.path / "trail.json"));
    corrupt["points"][0]["x"] = 30000000;
    atomicText(dir.path / "bad.json", corrupt.dump());
    rejects([&] { readTrail(dir.path / "bad.json", "world"); }, "Out-of-bounds stored point rejected");
    require(!trail.observe(0, std::numeric_limits<double>::quiet_NaN(), 0, 0, 27, true), "Nonfinite engine position ignored");
    ExplorationTrail bounded;
    for (int i = 0; i < 10000; ++i) bounded.observe(0, i * 2, 64, 0, i, true);
    require(bounded.points.size() <= ExplorationTrail::limit && bounded.points.size() > 7900
        && bounded.points.front().start && bounded.points.back().x == 19998, "Bounded history drops oldest points and preserves a valid start");
    auto revision = bounded.revision;
    bounded.clear();
    require(bounded.points.empty() && bounded.revision > revision, "Clear is a persistable change");
    writeTrail(dir.path / "cleared.json", "world", bounded.points);
    require(readTrail(dir.path / "cleared.json", "world").points.empty(), "Cleared history stays empty after restart");
}
void retraceTests() {
    ExplorationTrail trail;
    for (int i = 0; i < 5; ++i) trail.observe(0, i * 4, 64, 0, i, true);
    auto points = trail.points;
    require(!trail.startRetrace(1, 16, 64, 0), "Cannot retrace another dimension");
    require(!trail.startRetrace(0, 16, 0, 0), "Cannot attach to a trail on another floor");
    require(!trail.startRetrace(0, 100, 64, 0), "Cannot attach to a distant trail");
    require(trail.startRetrace(0, 16.5, 64, 0.5), "Nearby continuous route can be retraced");
    for (int i = 4; i >= 0; --i) {
        trail.observe(0, i * 4 + 0.5, 64, 0.5, 5 + (4 - i), true);
        if (i > 0) require(trail.retraceTarget()->x == (i - 1) * 4, "Retrace advances in reverse order");
    }
    require(!trail.retraceIndex() && trail.points == points, "Finishing stops at segment start without recording duplicates");
    require(trail.startRetrace(0, 16.5, 64, 0.5), "Can retrace again");
    trail.observe(0, 16.5, 64, 0.5, 20, true);
    trail.observe(0, 1000, 64, 0, 21, true);
    require(!trail.retraceIndex(), "Teleport cancels retracing");
    trail.observe(0, 1004, 64, 0, 22, true);
    trail.observe(0, 1008, 64, 0, 23, true);
    require(trail.startRetrace(0, 1008.5, 64, 0.5), "New segment is independently retraceable");
    trail.observe(0, 1008.5, 64, 0.5, 24, true);
    trail.observe(0, 1004.5, 64, 0.5, 25, true);
    require(!trail.retraceIndex(), "Retracing cannot cross a teleport gap");
    require(trail.startRetrace(0, 1008.5, 64, 0.5), "Can start before the next observation");
    trail.observe(0, 2000, 64, 0, 26, true);
    require(!trail.retraceIndex(), "Teleport on the very first retrace tick is also rejected");
}
void crossLayerTests(TestDirectory const& dir) {
    ExplorationTrail stairs;
    for (int i = 0; i <= 24; ++i) {
        int y = i - 12;
        stairs.observe({0, floorDiv(y, caveSliceHeight)}, i * 2, y, 0, i, true);
    }
    require(std::count_if(stairs.points.begin(), stairs.points.end(), [](auto const& p) { return p.start; }) == 1,
        "Walking stairs through positive and negative slices is one continuous route");
    auto original = stairs.points;
    auto path = dir.path / "stairs.json";
    writeTrail(path, "world", stairs.points);
    stairs = readTrail(path, "world");
    require(stairs.points == original, "V2 keeps cross-slice links after restart");
    require(stairs.startRetrace({0, 1}, 48.5, 12, 0.5), "Can attach to the end of a staircase");
    for (int i = 24; i >= 0; --i) {
        stairs.observe({0, floorDiv(i - 12, caveSliceHeight)}, i * 2 + 0.5, i - 12, 0.5, 30 + 24 - i, true);
        if (i > 0) require(stairs.retraceIndex() && stairs.retraceTarget()->x == (i - 1) * 2,
            "Retrace follows every stair point across slice changes");
    }
    require(!stairs.retraceIndex() && stairs.points == original, "Full staircase returns to its original start without extra recording");
    auto old = nlohmann::json::parse(std::ifstream(path));
    old["version"] = 1;
    atomicText(path, old.dump());
    auto legacy = readTrail(path, "world");
    require(std::count_if(legacy.points.begin(), legacy.points.end(), [](auto const& p) { return p.start; }) == 4,
        "Legacy files never invent previously unrecorded cross-slice links");
    ExplorationTrail entrance;
    entrance.observe(0, 0, 49, 0, 0, true);
    entrance.observe({0, 6}, 2, 48, 0, 1, true);
    entrance.observe({0, 5}, 4, 47, 0, 2, true);
    require(!entrance.points[1].start && !entrance.points[2].start, "Surface/cave auto mode changes retain movement continuity");
    require(entrance.startRetrace({0, 5}, 4.5, 47, 0.5), "Attach in cave");
    entrance.observe({0, 5}, 4.5, 47, 0.5, 3, true);
    entrance.observe({0, 6}, 2.5, 48, 0.5, 4, true);
    require(entrance.retraceTarget() && entrance.retraceTarget()->y == 49, "Cave route reaches original surface point");
    entrance.observe(0, 0.5, 49, 0.5, 5, true);
    require(!entrance.retraceIndex(), "Surface/cave switch does not stop retrace early");
    ExplorationTrail paused;
    paused.observe({0, -1}, 0, -1, 0, 0, true);
    for (int t = 1; t <= 20; ++t) paused.observe({0, -1}, 0, -1, 0, t, true);
    paused.observe({0, 0}, 2, 0, 0, 21, true);
    writeTrail(path, "world", paused.points);
    require(readTrail(path, "world").points == paused.points, "Standing still creates no false stored timestamp gap");
    paused.breakSegment();
    paused.observe({0, 0}, 4, 1, 0, 22, true);
    require(paused.points.back().start, "Explicit death/focus/teleport break still wins over cross-slice continuity");
    paused.observe({1, 0}, 6, 1, 0, 23, true);
    require(paused.points.back().start, "Same-height portal change is still a break");

    TrailPoint a{{0, -1}, 0, -2, 0, 0, true}, b{{0, 0}, 8, 2, 0, 1, false};
    MapView view{4.5, 0.5, 1, 20, 20}; MapRect area{0, 0, 200, 200};
    auto lower = projectTrailLine(a, b, {0, -1}, view, area);
    auto upper = projectTrailLine(a, b, {0, 0}, view, area);
    require(lower && upper && std::abs(lower->x2 - upper->x1) < 0.01,
        "Cross-slice polyline clips at the actual Y boundary");
    require(!projectTrailLine(a, b, {0, 1}, view, area), "Stair line does not leak into an unrelated height slice");
    a.layer = b.layer = MapLayer{0};
    require(!projectTrailLine(a, b, {0, 0}, view, area), "Surface trails do not appear on unvisited underground floors");
}
void viewAndMenuTests(TestDirectory const& dir) {
    Settings settings; Navigation nav; WayfinderView view;
    view.fullscreen = true; view.layout(500, 300, settings, 20, 30);
    view.selectLayer({1, -3});
    require(view.displayedLayer({0, 4}) == MapLayer(1, -3) && !view.following, "Locked layer independent of live sampling layer");
    view.layout(500, 300, settings, 100, 100);
    require(view.lockedLayer == MapLayer(1, -3), "Rendering while moving does not unlock history");
    view.fullscreen = false;
    require(view.displayedLayer({0, 4}) == MapLayer(0, 4), "Minimap always uses live layer");
    view.fullscreen = true; view.follow(100, 100);
    require(!view.lockedLayer && view.displayedLayer({0, 4}) == MapLayer(0, 4), "Home restores live layer and position");
    MapMenu menu;
    menu.knownLayers = {{0}, {0, -1}, {1, 8}};
    menu.trailPoints = 10;
    require(menu.action({{}, {}, UiAction::Explore}, settings, nav) == 0 && menu.page == MapMenu::Page::Explore, "Toolbar opens exploration page");
    menu.action({{}, {}, UiAction::RecordTrail}, settings, nav);
    menu.action({{}, {}, UiAction::ShowTrail}, settings, nav);
    saveSettings(dir.path / "config", settings);
    auto loaded = loadSettings(dir.path / "config");
    require(!loaded.recordTrail && !loaded.showTrail, "Trail settings roundtrip");
    atomicText(dir.path / "old/config.json", "{}");
    loaded = loadSettings(dir.path / "old");
    require(loaded.recordTrail && loaded.showTrail, "Old settings receive trail defaults");
    for (float width : {112.0f, 359.0f, 419.0f, 420.0f, 800.0f}) {
        menu.open(MapMenu::Page::Map);
        auto frame = menu.build(width, 144, settings, nav);
        require(frame.buttons.size() == 6, "All toolbar actions present at every width");
        for (auto const& b : frame.buttons)
            require(b.rect.x >= 0 && b.rect.x + b.rect.width <= width
                && b.rect.y + b.rect.height <= mapToolbarBottom(width), "Toolbar never overlaps map");
    }
    menu.open(MapMenu::Page::Explore);
    auto frame = menu.build(112, 144, settings, nav);
    require(menu.visibleRows >= 1 && !frame.pagination.empty(), "Exploration remains usable at minimum window size");
    menu.context(Waypoint{}, 10, 10); menu.draft.y = 64; menu.contextCanTeleport = false;
    frame = menu.build(420, 280, settings, nav);
    for (auto const& b : frame.buttons)
        if (b.action == UiAction::Teleport) require(!b.enabled, "Remote-dimension history cannot teleport the player");
    TrailPoint a{{0}, -1000, 64, 0, 0, true}, b{{0}, 1000, 64, 0, 1, false};
    auto line = projectTrailLine(a, b, 0, MapView{0, 0, 1, 100, 100}, {10, 20, 200, 200});
    require(line && line->x1 == 10 && line->x2 == 210 && line->y1 >= 20 && line->y2 <= 220, "Trail crossing viewport is clipped");
    require(!projectTrailLine(a, b, 1, MapView{}, {0, 0, 100, 100}), "Other-layer trails are hidden");
    b.start = true;
    require(!projectTrailLine(a, b, 0, MapView{}, {0, 0, 100, 100}), "No line across a segment break");
    std::vector<std::uint32_t> terrain(10000, rgba(80, 120, 70));
    auto original = terrain;
    std::array<TrailPoint, 2> segment{a, b};
    overlayTrail(terrain, MapView{0, 0, 1, 100, 100}, 0, segment);
    require(terrain == original, "Raster overlay does not bridge a break");
    segment[1].start = false;
    overlayTrail(terrain, MapView{0, 0, 1, 100, 100}, 1, segment);
    require(terrain == original, "Raster overlay hides other dimensions");
    overlayTrail(terrain, MapView{0, 0, 1, 100, 100}, 0, segment);
    require(terrain != original && terrain.front() == original.front() && terrain.back() == original.back(),
        "Raster trail paints clipped line without touching unrelated pixels");
    require(std::count_if(terrain.begin(), terrain.end(), [&](auto p) { return p != original[0]; }) == 100,
        "Horizontal trail covers the full clipped viewport in one texture");
    terrain = original;
    segment[0].z = -1000; segment[1].z = 1000;
    overlayTrail(terrain, MapView{0, 0, 1, 100, 100}, 0, segment);
    for (int i = 0; i < 100; ++i) require(terrain[i * 100 + i] != original[0], "Diagonal raster trail has no holes");
    for (int density : {2, 3, 4}) {
        int side = 100 * density;
        std::vector<std::uint32_t> detailed(side * side, original[0]);
        overlayTrail(detailed, MapView{0, 0, 1.0 / density, side, side}, 0, segment, density);
        bool matches = true;
        for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x)
            matches = matches && detailed[y * side + x] == terrain[(y / density) * 100 + x / density];
        require(matches, "All terrain densities preserve trail position, width and blending");
    }
}
} // namespace
int main() {
    try {
        TestDirectory dir;
        heightTests(); trailTests(dir); retraceTests(); crossLayerTests(dir); viewAndMenuTests(dir);
        std::cout << checks << " checks passed: height navigation, trails, retracing, history layers and UI.\n";
    } catch (std::exception const& ex) { std::cerr << "FAILED after " << checks << " checks: " << ex.what() << "\n"; return 1; }
}
