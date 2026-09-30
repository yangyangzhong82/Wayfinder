#include "wayfinder/CoordinateInput.h"
#include "wayfinder/MapMenu.h"
#include "wayfinder/MapMarkers.h"
#include "wayfinder/NativeTextInput.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

using namespace wayfinder;
namespace {
int checks{};
void require(bool ok, char const* message) { ++checks; if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f, char const* message) {
    bool threw{}; try { f(); } catch (std::exception const&) { threw = true; } require(threw, message);
}
struct TestDirectory {
    std::filesystem::path path = std::filesystem::path("build/tests")
        / ("interaction-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TestDirectory() { std::filesystem::create_directories(path); }
    ~TestDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
void coordinates() {
    Waypoint p; p.x = 4; p.y = 60; p.z = 9;
    auto point = withCoordinates(p, "  -120 64 +300\r\n");
    require(point.x == -120 && point.y == 64 && point.z == 300, "XYZ paste handles signed coordinates and clipboard whitespace");
    point = withCoordinates(p, "-120, -300");
    require(point.x == -120 && point.z == -300 && point.y == 60, "XZ paste preserves existing optional Y");
    p.y.reset();
    require(!withCoordinates(p, "1 2").y, "XZ does not invent a terrain height");
    for (auto value : {"", "1", "1 2 3 4", "1.5 2", "1 x 2", "--1 2", "+-1 2", "2147483648 0", "30000000 0", "0 -32769 0"})
        rejects([&] { withCoordinates(p, value); }, "Invalid coordinate tuple rejected");
    MapMenu menu;
    menu.edit(p);
    menu.beginField(UiAction::CoordinateText);
    menu.text = "120 64 -300";
    menu.commitField();
    require(menu.draft.x == 120 && menu.draft.y == 64 && menu.draft.z == -300, "Menu applies XYZ as one validated change");
    auto before = menu.draft;
    menu.beginField(UiAction::X);
    menu.text = "5 999999 20";
    rejects([&] { menu.commitField(); }, "Invalid Y prevents every part of a coordinate edit");
    require(menu.draft == before && menu.focus == UiAction::X && !menu.text.empty(), "Failed edit retains original draft and editable text");
    menu.text = "-2, 70, 8";
    menu.commitField();
    require(menu.draft.x == -2 && menu.draft.y == 70 && menu.draft.z == 8, "Individual coordinate field also accepts an entire tuple");
    menu.beginField(UiAction::Y); menu.text = " "; menu.commitField();
    require(!menu.draft.y, "Clearing optional Y restores unknown height");
    menu.beginField(UiAction::Name); menu.text = "矿洞入口😀"; menu.commitField();
    require(menu.draft.name == "矿洞入口😀", "Native committed Unicode including supplementary characters is preserved");
    menu.beginField(UiAction::Name); menu.text = "取消的输入"; menu.cancelField();
    require(menu.draft.name == "矿洞入口😀", "Cancelling native field leaves draft intact");
    menu.beginField(UiAction::Name); menu.text = "";
    rejects([&] { menu.commitField(); }, "Empty name is corrected before save");
}
void groups(TestDirectory const& dir) {
    Navigation nav; Waypoint p; p.group = "基地"; p.name = "基地 A"; p.x = -10; auto base = nav.save(p);
    p.id = 0; p.group = "矿点"; p.name = "矿点 B"; p.x = 10; auto mine = nav.save(p);
    p.id = 0; p.group.clear(); p.name = "未分组"; p.z = 10; auto other = nav.save(p);
    nav.toggleGroup("基地", false);
    nav.toggleGroup("矿点", true);
    require(!nav.groupVisible("基地", false) && nav.groupVisible("基地", true), "Minimap visibility independent of full map");
    MapView view{0, 0, 1, 128, 128}; MapRect area{0, 0, 256, 256};
    auto minimapMarkers = layoutMarkers(nav, view, area, 0, true, true, false, {}, false);
    auto full = layoutMarkers(nav, view, area, 0, true, true, true, {}, true);
    auto contains = [](auto const& points, auto id) { return std::any_of(points.begin(), points.end(), [&](auto const& p) { return p.id == id; }); };
    require(!contains(minimapMarkers, base) && contains(full, base) && contains(minimapMarkers, mine) && !contains(full, mine), "Rendering applies the correct map's group filter");
    auto projection = projectMarker(view, p.x + 0.5, 0.5, area.width, area.height);
    require(hitMarker(full, float(projection.x), float(projection.y)) == 0, "Hidden markers are also absent from mouse hit testing");
    nav.target = mine;
    full = layoutMarkers(nav, view, area, 0, true, true, true);
    require(full.back().id == mine && full.back().target, "Selected target overrides group hiding and draws last");
    nav.toggleGroup("", false);
    require(!nav.groupVisible("", false), "Ungrouped markers have their own visibility switch");
    nav.points[1].favorite = true;
    WaypointQuery query; query.sort = WaypointQuery::Sort::Name;
    require(queryWaypoints(nav, query).front()->id == mine, "Favorites precede ordinary list sorting");
    query.group = "基地";
    require(queryWaypoints(nav, query).size() == 1, "Favorites still obey list filters");
    auto file = dir.path / "waypoints.json";
    writeNavigation(file, "world", nav);
    auto read = readNavigation(file, "world");
    require(read.points == nav.points && read.hiddenMinimapGroups == nav.hiddenMinimapGroups
        && read.hiddenFullMapGroups == nav.hiddenFullMapGroups, "Groups and favorites persist together per world");
    rejects([&] { readNavigation(file, "different-world"); }, "Visibility never crosses world identity");
    auto json = nlohmann::json::parse(std::ifstream(file));
    json.erase("hiddenMinimapGroups"); json.erase("hiddenFullMapGroups");
    for (auto& point : json["points"]) point.erase("favorite");
    atomicText(file, json.dump());
    auto old = readNavigation(file, "world");
    require(old.hiddenMinimapGroups.empty() && old.hiddenFullMapGroups.empty()
        && std::none_of(old.points.begin(), old.points.end(), [](auto const& p) { return p.favorite; }), "Old waypoints default to visible and not favorite");
    json["hiddenFullMapGroups"] = nlohmann::json::array({" invalid "});
    atomicText(file, json.dump());
    rejects([&] { readNavigation(file, "world"); }, "Invalid persisted group names rejected");

    MapMenu menu; Settings settings;
    menu.open(MapMenu::Page::List);
    menu.action({{}, {}, UiAction::BatchMode}, settings, nav);
    menu.action({{}, {}, UiAction::SelectWaypoint, base}, settings, nav);
    menu.action({{}, {}, UiAction::SelectWaypoint, other}, settings, nav);
    menu.beginField(UiAction::BatchGroup); menu.text = "  施工  "; menu.commitField();
    auto copy = nav;
    require(menu.action({{}, {}, UiAction::ApplyGroup}, settings, copy) == 1, "Bulk regroup requests immediate navigation persistence");
    require(copy.find(base)->group == "施工" && copy.find(other)->group == "施工" && copy.find(mine)->group == "矿点", "Bulk edit only touches selected stable IDs");
    require(nav.find(base)->group == "基地", "Caller can retain original navigation on save failure");
    menu.open(MapMenu::Page::Groups);
    for (float width : {112.0f, 300.0f, 800.0f}) {
        auto frame = menu.build(width, 144, settings, nav);
        require(!frame.pagination.empty() && menu.visibleRows > 0, "Group controls remain reachable by paging on small windows");
        for (auto const& b : frame.buttons)
            require(b.rect.x >= 0 && b.rect.x + b.rect.width <= width, "Group controls stay within horizontal bounds");
    }
    UiButton toggle{{}, {}, UiAction::FullMapGroup}; toggle.data = "基地";
    require(menu.action(toggle, settings, nav) == 1 && !nav.groupVisible("基地", true), "Group button uses stable group name and requests saving");
}
void nativeCancellation() {
    NativeTextInput input;
    for (int i = 0; i < 8; ++i) {
        input.start({"Wayfinder input cancellation test", "中文测试", "", "OK", "Cancel", {}});
        input.cancel();
        std::optional<NativeTextInput::Result> result;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!(result = input.take()) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        require(result && !result->accepted && result->error.empty() && !input.active(),
            "Native dialog template creates successfully; startup cancellation safely joins the UI thread");
    }
    input.start({"Wayfinder shutdown test", "", "", "OK", "Cancel", {}});
    input.shutdown();
    require(!input.active(), "Plugin shutdown drains pending editor startup");
}
} // namespace
int main() {
    try {
        TestDirectory dir; coordinates(); groups(dir); nativeCancellation();
        std::cout << checks << " checks passed: coordinates, groups, favorites, batch edits and native input lifecycle.\n";
    } catch (std::exception const& ex) { std::cerr << "FAILED after " << checks << " checks: " << ex.what() << '\n'; return 1; }
}
