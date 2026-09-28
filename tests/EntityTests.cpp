#include "wayfinder/EntityRadar.h"
#include "wayfinder/MapMenu.h"
#include "wayfinder/PortraitPixels.h"
#include "wayfinder/PortraitDraw.h"
#include <chrono>
#include <fstream>
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
        / ("entity-regression-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TestDirectory() { std::filesystem::create_directories(path); }
    ~TestDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
UiButton action(UiAction type, std::string data = {}) {
    UiButton result; result.action = type; result.data = std::move(data); return result;
}
void settingsTests(TestDirectory const& dir) {
    require(entityTypeId(" Zombie ") == "minecraft:zombie", "Short identifiers normalize to the vanilla namespace");
    require(entityTypeId("Addon:Forest/Fox") == "addon:forest/fox", "Custom entity identifiers retain their namespace");
    for (auto bad : {"", " ", ":zombie", "minecraft:", "minecraft:zombie:*", "minecraft:*", "zom bie", "僵尸"})
        rejects([&] { entityTypeId(bad); }, "Malformed identifiers are rejected");
    Settings s;
    require(s.entityPortraits, "Portraits default on");
    require(s.minimapEntityScale == 0.75f && s.undergroundEntityOpacity == 0.35f && s.entityDepthThreshold == 4,
            "Smaller minimap icons and height fading have compatible defaults");
    require(s.showEntities && !s.entityFilterEnabled && s.entityRadius == 64, "Default radar shows all nearby types");
    s.showEntities = false; s.entityFilterEnabled = true; s.entityRadius = 96;
    s.entityPortraits = false;
    s.minimapEntityScale = 1.25f; s.undergroundEntityOpacity = 0.6f; s.entityDepthThreshold = 8;
    s.entityTypes = {"Zombie", "minecraft:zombie", "PLAYER", "addon:guard"};
    saveSettings(dir.path, s);
    auto loaded = loadSettings(dir.path);
    require(!loaded.entityPortraits, "Portrait preference persists");
    require(loaded.minimapEntityScale == 1.25f && loaded.undergroundEntityOpacity == 0.6f && loaded.entityDepthThreshold == 8,
            "Icon size, lower opacity and relative height threshold persist");
    require(!loaded.showEntities && loaded.entityFilterEnabled && loaded.entityRadius == 96, "Radar toggles and radius persist");
    require(loaded.entityTypes == std::vector<std::string>{"addon:guard", "minecraft:player", "minecraft:zombie"},
            "Selected types normalize, deduplicate and persist");
    require(allowedEntityType(loaded, "minecraft:zombie") && !allowedEntityType(loaded, "minecraft:husk"),
            "Whitelist uses exact types rather than partial matches or related mobs");
    loaded.entityTypes.clear();
    saveSettings(dir.path, loaded);
    loaded = loadSettings(dir.path);
    require(loaded.entityFilterEnabled && !allowedEntityType(loaded, "minecraft:player"),
            "An enabled empty selection remains empty after reload");
    auto json = nlohmann::json::parse(std::ifstream(dir.path / "config.json"));
    for (auto field : {"showEntities", "entityPortraits", "entityFilterEnabled", "entityRadius", "entityTypes",
        "minimapEntityScale", "undergroundEntityOpacity", "entityDepthThreshold"}) json.erase(field);
    atomicText(dir.path / "config.json", json.dump());
    loaded = loadSettings(dir.path);
    require(loaded.entityPortraits, "Old configurations enable portraits by default");
    require(loaded.minimapEntityScale == 0.75f && loaded.undergroundEntityOpacity == 0.35f && loaded.entityDepthThreshold == 4,
            "Old configurations receive scale and fading defaults");
    require(loaded.showEntities && !loaded.entityFilterEnabled && loaded.entityRadius == 64 && loaded.entityTypes.empty(),
            "Old configs receive compatible radar defaults");
    s = loaded; s.entityRadius = -100; normalizeSettings(s);
    require(s.entityRadius == 16, "Radius clamps to its lower limit");
    s.entityRadius = 1000; normalizeSettings(s);
    require(s.entityRadius == 256, "Radius clamps to its upper limit");
    s.minimapEntityScale = -1; s.undergroundEntityOpacity = -1; s.entityDepthThreshold = -1; normalizeSettings(s);
    require(s.minimapEntityScale == 0.25f && s.undergroundEntityOpacity == 0.1f && s.entityDepthThreshold == 1,
            "Scale, opacity and depth clamp to safe lower bounds");
    s.minimapEntityScale = 100; s.undergroundEntityOpacity = 100; s.entityDepthThreshold = 100; normalizeSettings(s);
    require(s.minimapEntityScale == 2 && s.undergroundEntityOpacity == 1 && s.entityDepthThreshold == 64,
            "Scale, opacity and depth clamp to upper bounds");
    for (auto value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        auto bad = s; bad.minimapEntityScale = value;
        rejects([&] { saveSettings(dir.path, bad); }, "Non-finite icon sizes cannot replace valid settings");
        bad = s; bad.undergroundEntityOpacity = value;
        rejects([&] { saveSettings(dir.path, bad); }, "Non-finite opacity cannot replace valid settings");
    }
    s.entityTypes = {"invalid type"};
    rejects([&] { saveSettings(dir.path, s); }, "Invalid filters cannot replace a valid config");
    require(loadSettings(dir.path).entityTypes.empty(), "Failed saves retain the previous filter");
    s.entityTypes.assign(257, "minecraft:player");
    rejects([&] { normalizeSettings(s); }, "Entity selection size is bounded");
    s = loaded; s.showEntities = false; s.entityPortraits = false; s.entityFilterEnabled = true; s.entityTypes = {"minecraft:player"};
    s.minimapEntityScale = 2; s.undergroundEntityOpacity = 1; s.entityDepthThreshold = 64;
    resetMapPreferences(s);
    require(s.entityPortraits, "Reset restores portraits");
    require(s.minimapEntityScale == 0.75f && s.undergroundEntityOpacity == 0.35f && s.entityDepthThreshold == 4,
            "Reset restores scale and height fading defaults");
    require(s.showEntities && !s.entityFilterEnabled && s.entityTypes.empty() && s.entityRadius == 64,
            "Reset restores radar defaults");
}
void menuTests() {
    Settings s; MapMenu menu; Navigation nav;
    menu.open(MapMenu::Page::Settings); menu.settingsTab = 1;
    auto frame = menu.build(420, 280, s, nav);
    for (auto type : {UiAction::Entities, UiAction::ShowEntities})
        require(std::any_of(frame.buttons.begin(), frame.buttons.end(), [&](auto const& b) { return b.action == type; }),
                "The radar switch and filter page are reachable from map settings");
    require(menu.action(action(UiAction::Entities), s, nav) == 0 && menu.page == MapMenu::Page::Entities,
            "Entity settings open without modifying stored settings");
    menu.loadedEntities = 17; menu.nearbyEntities = 9; menu.visibleEntities = 3;
    frame = menu.build(420, 280, s, nav);
    require(frame.message == "Entities loaded: 17 | Nearby: 9 | Matched: 3",
            "Entity settings expose collection and filter counts for live diagnosis");
    require(menu.action(action(UiAction::ShowEntities), s, nav) == 2 && !s.showEntities, "Master switch requests a save");
    menu.action(action(UiAction::ShowEntities), s, nav);
    require(menu.action(action(UiAction::EntityPortraits), s, nav) == 2 && !s.entityPortraits && s.showEntities,
            "Portrait toggle saves independently of master entity visibility");
    menu.action(action(UiAction::EntityPortraits), s, nav);
    require(menu.action(action(UiAction::EntityScaleDown), s, nav) == 2 && std::abs(s.minimapEntityScale - 0.7f) < 0.00001f,
            "Scale decrement saves a five percent decrease");
    menu.action(action(UiAction::EntityScaleUp), s, nav);
    require(std::abs(s.minimapEntityScale - 0.75f) < 0.00001f, "Scale increment restores five percent");
    menu.action(action(UiAction::EntityTransparencyUp), s, nav);
    require(std::abs(s.undergroundEntityOpacity - 0.3f) < 0.00001f, "Higher transparency means lower opacity");
    menu.action(action(UiAction::EntityTransparencyDown), s, nav);
    require(std::abs(s.undergroundEntityOpacity - 0.35f) < 0.00001f, "Lower transparency means higher opacity");
    menu.action(action(UiAction::EntityDepthUp), s, nav);
    require(s.entityDepthThreshold == 5, "Height threshold increases one block");
    menu.action(action(UiAction::EntityDepthDown), s, nav);
    require(s.entityDepthThreshold == 4, "Height threshold decreases one block");
    frame = menu.build(420, 400, s, nav);
    for (auto const& expected : {std::pair{"Minimap entity scale", "75%"}, {"Lower entity transparency", "65%"}})
        require(std::any_of(frame.buttons.begin(), frame.buttons.end(), [&](auto const& b) {
            return b.text == expected.first && b.value == expected.second;
        }), "Entity controls show scale and transparency as human-readable percentages");
    require(menu.action(action(UiAction::EntityType, "minecraft:zombie"), s, nav) == 2
        && s.entityFilterEnabled && allowedEntityType(s, "minecraft:zombie") && !allowedEntityType(s, "minecraft:player"),
        "Selecting a type switches directly to a whitelist");
    menu.action(action(UiAction::EntityType, "minecraft:player"), s, nav);
    require(s.entityTypes.size() == 2 && allowedEntityType(s, "minecraft:player"), "Multiple entity types can be selected");
    menu.action(action(UiAction::EntityType, "minecraft:zombie"), s, nav);
    require(s.entityTypes == std::vector<std::string>{"minecraft:player"}, "Unchecking removes only the selected type");
    menu.action(action(UiAction::EntityAll), s, nav);
    require(!s.entityFilterEnabled && allowedEntityType(s, "addon:guard"), "All types includes custom entities");
    menu.action(action(UiAction::EntityFilterMode), s, nav);
    require(s.entityFilterEnabled && s.entityTypes.size() == 1, "Filter mode preserves the saved selection");
    menu.action(action(UiAction::EntityNone), s, nav);
    require(s.entityFilterEnabled && s.entityTypes.empty() && !allowedEntityType(s, "minecraft:player"),
            "Clearing selection does not accidentally display everything");
    menu.action(action(UiAction::EntityPlayersOnly), s, nav);
    require(s.entityTypes == std::vector<std::string>{"minecraft:player"} && s.entityFilterEnabled, "Players-only preset is exact");
    menu.action(action(UiAction::EntityRadiusUp), s, nav);
    require(s.entityRadius == 80, "Entity radius increases by sixteen blocks");
    menu.action(action(UiAction::EntityRadiusDown), s, nav);
    require(s.entityRadius == 64, "Entity radius decreases by sixteen blocks");
    menu.observedEntityTypes = {"addon:guard", "minecraft:zombie", "addon:guard"};
    auto choices = entityTypeChoices(menu.observedEntityTypes, s);
    require(choices.front() == "minecraft:player" && std::count(choices.begin(), choices.end(), "addon:guard") == 1,
            "Observed custom entities join the catalog once, with players first");
    menu.locale.select("zh_CN", "en_US");
    menu.action(action(UiAction::EntitySearch), s, nav); menu.append("僵尸"); menu.commitField();
    require(menu.entitySearch == "僵尸" && entityTypeMatches("minecraft:zombie", menu.entitySearch, menu.locale),
            "Entity search supports localized Chinese names");
    require(entityTypeMatches("minecraft:zombie", "ZOMBIE", menu.locale), "Identifier search ignores ASCII case");
    menu.entitySearch = "addon:guard"; menu.offset = 0;
    frame = menu.build(420, 400, s, nav);
    auto custom = std::find_if(frame.buttons.begin(), frame.buttons.end(), [](auto const& b) { return b.action == UiAction::EntityType; });
    for (int page = 0; custom == frame.buttons.end() && page < 4; ++page) {
        menu.action(action(UiAction::Next), s, nav); frame = menu.build(420, 400, s, nav);
        custom = std::find_if(frame.buttons.begin(), frame.buttons.end(), [](auto const& b) { return b.action == UiAction::EntityType; });
    }
    require(custom != frame.buttons.end() && custom->data == "addon:guard", "Custom entity buttons carry a stable identifier");
    auto selected = *custom;
    menu.observedEntityTypes.push_back("addon:aaa"); menu.entitySearch.clear();
    menu.action(selected, s, nav);
    require(selectedEntityType(s, "addon:guard") && !selectedEntityType(s, "addon:aaa"),
            "Catalog reordering cannot change a clicked button's entity type");
    frame = menu.build(112, 144, s, nav);
    require(menu.visibleRows >= 1 && !frame.pagination.empty(), "Small windows retain entity pagination");
    for (int page = 0; page < 4; ++page) {
        for (auto const& b : frame.buttons) require(b.rect.x >= 0 && b.rect.y >= 0
            && b.rect.x + b.rect.width <= 112 && b.rect.y + b.rect.height <= 144, "Entity controls stay inside small windows");
        menu.action(action(UiAction::Next), s, nav); frame = menu.build(112, 144, s, nav);
    }
    menu.action(action(UiAction::Back), s, nav);
    require(menu.page == MapMenu::Page::Settings && menu.settingsTab == 1, "Back returns to the map-layer settings");
    auto builtins = entityTypeChoices({}, Settings{});
    for (auto const& type : builtins) require(entityTypeLabel(type, menu.locale) != type, "Built-in types have translated labels");
}
void visibilityTests() {
    Settings s; Locale locale;
    std::vector<EntityMarker> entities{
        {0, 2, 64, 3, "minecraft:player", "Alex", true},
        {0, -4, 64, -5, "minecraft:zombie", {}, false},
        {1, 2, 64, 3, "minecraft:player", "Other dimension", true},
        {0, 80, 64, 0, "minecraft:player", "Too far", true}};
    MapView view{0, 0, 1, 128, 128}; MapRect area{10, 20, 128, 128};
    auto markers = layoutEntities(entities, view, area, 0, 0, 64, 0, s, locale);
    require(markers.size() == 2 && markers[0].name == "Alex" && markers[1].name.empty(),
            "Radar renders same-dimension nearby entities and names players");
    require(markers[0].type == "minecraft:player" && markers[1].type == "minecraft:zombie",
            "Projection retains the exact type needed for portrait selection");
    require(markers[0].anchorX == 76 && markers[0].anchorY == 87 && markers[1].anchorX == 70,
            "Entities use the map's world projection including negative coordinates");
    s.showEntities = false;
    require(layoutEntities(entities, view, area, 0, 0, 64, 0, s, locale).empty(), "Disabling immediately hides an existing snapshot");
    s.showEntities = true; s.entityFilterEnabled = true; s.entityTypes = {"minecraft:zombie"};
    markers = layoutEntities(entities, view, area, 0, 0, 64, 0, s, locale);
    require(markers.size() == 1 && !markers[0].player, "Rendering applies whitelist changes to the current snapshot");
    s.entityTypes = {"minecraft:player"};
    require(layoutEntities(entities, view, area, 0, 0, 64, 0, s, locale).size() == 1, "Players-only excludes mobs and other dimensions");
    s.entityTypes.clear();
    require(layoutEntities(entities, view, area, 0, 0, 64, 0, s, locale).empty(), "Empty selection hides cached markers");
    s.entityFilterEnabled = false;
    EntityMarker e{0, 0, 80, 0, "minecraft:zombie", {}, false};
    require(nearbyEntity(e, {0, 8}, 0, 64, 0, 64), "Cave radar includes a sixteen-block vertical boundary");
    e.y = 81;
    require(!nearbyEntity(e, {0, 8}, 0, 64, 0, 64) && nearbyEntity(e, 0, 0, 64, 0, 64),
            "Cave radar excludes entities on distant vertical floors");
    e.x = 64; e.y = 64;
    require(nearbyEntity(e, 0, 0, 64, 0, 64), "The radius boundary is included");
    e.z = 1; require(!nearbyEntity(e, 0, 0, 64, 0, 64), "Radar radius is spherical, not the query box");
    e.x = std::numeric_limits<double>::quiet_NaN();
    require(!nearbyEntity(e, 0, 0, 64, 0, 64), "Invalid entity positions are ignored");
    view.centerX = 1000;
    require(layoutEntities(entities, view, area, 0, 0, 64, 0, s, locale).empty(), "Panning to history never pins live entities to an edge");
    view = {-20, -20, 0.5, 64, 64}; area = {4, 5, 64, 64};
    entities = {{0, -35.9, 64, -35.9, "minecraft:player", "玩家测试", true},
                {0, -4.1, 64, -4.1, "minecraft:player", std::string(32, 'W'), true},
                {0, -20, 64, -20, "minecraft:player", {}, true}};
    markers = layoutEntities(entities, view, area, 0, -20, 64, -20, s, locale);
    require(markers.size() == 3 && markers[0].name == "玩家测试" && markers[2].name == "Player",
            "Unicode player names survive and missing names have a visible fallback");
    for (auto const& m : markers) require(!m.name.empty() && m.label.x >= area.x && m.label.y >= area.y
        && m.label.x + m.label.width <= area.x + area.width && m.label.y + m.label.height <= area.y + area.height
        && m.textScale > 0 && m.textScale <= 0.6f, "Player labels fit the map even at its edges");
    require(markers[1].textScale < 0.6f, "Long names shrink to fit the minimap");
    auto crowdedPlayer = entities.front();
    entities.assign(8, crowdedPlayer);
    markers = layoutEntities(entities, view, area, 0, -20, 64, -20, s, locale);
    require(markers.size() == 8 && std::all_of(markers.begin(), markers.end(), [](auto const& m) { return !m.name.empty() && m.label.width > 0; }),
            "Crowded players keep their names instead of silently losing labels");
}
void capacityTests() {
    std::vector<EntityMarker> heap;
    for (int i = 700; i >= 1; --i) keepNearbyEntity(heap, {0, double(i), 64, 0, "minecraft:cow", {}, false}, 0, 64, 0);
    require(heap.size() == 512, "Snapshots have a bounded marker count");
    require(std::all_of(heap.begin(), heap.end(), [](auto const& e) { return e.x <= 512; }), "Nearest non-player entities survive overflow");
    keepNearbyEntity(heap, {0, 1000, 64, 0, "minecraft:player", "Priority player", true}, 0, 64, 0);
    require(heap.size() == 512 && std::count_if(heap.begin(), heap.end(), [](auto const& e) { return e.player; }) == 1,
            "Players retain priority over crowded mob farms");
    keepNearbyEntity(heap, {0, 2000, 64, 0, "minecraft:cow", {}, false}, 0, 64, 0);
    require(std::none_of(heap.begin(), heap.end(), [](auto const& e) { return e.x == 2000; }), "Distant entities cannot evict nearer ones");
}
void portraitTests() {
    std::array<std::uint8_t, 12> masked{{224,121,250,3, 142,114,94,3, 255,255,255,0}};
    auto pixels = portraitPixels(masked, 3, 1, 4);
    require(pixels.pixels.size() == 3 && pixels.pixels[0] == rgba(224,121,250)
        && pixels.pixels[1] == rgba(142,114,94) && (pixels.pixels[2] >> 24) == 0,
        "Emissive eyes and tint-masked faces stay visible while transparent gutters remain clear");
    std::array<std::uint8_t, 4> bgra{{10,20,30,128}};
    require(portraitPixels(bgra,1,1,4,true).pixels[0] == rgba(30,20,10),
        "BGRA texture caches convert to RGBA without swapped face colors");
    require(portraitPixels(std::span(masked).first(3),1,1,3).pixels[0] == rgba(224,121,250),
        "RGB texture caches receive opaque alpha");
    require(portraitPixels(masked,4,4,4).pixels.empty()
        && portraitPixels(masked,0,1,4).pixels.empty() && portraitPixels(masked,1,1,2).pixels.empty(),
        "Invalid dimensions, short image buffers and unsupported formats fail safely");
    std::vector<std::uint8_t> highResolution(512*256*4,255);
    auto small = portraitPixels(highResolution,512,256,4);
    require(small.width == 256 && small.height == 128 && small.pixels.size() == 256*128,
        "High-resolution pack copies have bounded memory and retain aspect ratio");
    std::set<std::string_view> types;
    for (auto const& portrait : entityPortraitCatalog) {
        require(types.insert(portrait.type).second, "Portrait type identifiers are unique");
        require(portrait.texture.starts_with("textures/entity/") && !portrait.parts.empty(),
                "Portraits reference entity textures and contain drawable faces");
        require(entityPortrait(portrait.type) == &portrait, "Exact type resolves its own portrait");
        for (auto const& part : portrait.parts) {
            require(std::isfinite(part.u) && std::isfinite(part.v) && part.uw != 0 && part.vh != 0
                && std::min(part.u, part.u + part.uw) >= -0.00001f
                && std::max(part.u, part.u + part.uw) <= 1.00001f
                && std::min(part.v, part.v + part.vh) >= -0.00001f
                && std::max(part.v, part.v + part.vh) <= 1.00001f,
                "Mirrored and standard UV rectangles stay inside their texture");
            require(part.width > 0 && part.height > 0 && part.x >= -0.00001f && part.y >= -0.00001f
                && part.x + part.width <= 1.00001f && part.y + part.height <= 1.00001f,
                "Head, muzzle and ear layers fit the portrait without stretching");
        }
    }
    for (auto type : {"minecraft:zombie", "minecraft:creeper", "minecraft:cow", "minecraft:sheep",
        "minecraft:piglin", "minecraft:ghast", "minecraft:enderman", "minecraft:shulker"})
        require(entityPortrait(type) != nullptr, "Common Overworld, Nether and End mobs have portraits");
    require(!entityPortrait("addon:cow") && !entityPortrait("minecraft:item") && !entityPortrait("minecraft:player"),
            "Custom entities and non-portrait types retain their markers instead of a misleading head");
    auto const& creeper = entityPortrait("minecraft:creeper")->parts.front();
    require(creeper.u == 0.125f && creeper.v == 0.25f && creeper.uw == 0.125f && creeper.vh == 0.25f,
            "Creeper profile selects the face rather than its body or entire texture");
    Settings s; s.minimapEntityScale = 1; Locale locale;
    std::vector<EntityMarker> nearby{{0,0,64,0,"minecraft:player","Alex",true},
        {0,8,64,0,"minecraft:zombie",{},false}};
    auto withPortraits = layoutEntities(nearby, {0,0,1,128,128}, {0,0,128,128}, 0, 0,64,0,s,locale);
    require(withPortraits.size() == 2 && !withPortraits[0].label.intersects(entityVisualBounds(withPortraits[1], s)),
            "Player names avoid the larger neighboring portrait bounds");
    s.entityPortraits = false;
    auto dots = layoutEntities(nearby, {0,0,1,128,128}, {0,0,128,128}, 0, 0,64,0,s,locale);
    require(dots.size() == withPortraits.size() && dots[0].name == "Alex" && dots[1].anchorX == withPortraits[1].anchorX,
            "Disabling portraits preserves entities, names and geographic positions");
}
void appearanceTests() {
    Settings s; Locale locale;
    std::vector<EntityMarker> entities{
        {0, -20, 60, 0, "minecraft:zombie", {}, false},
        {0, 0, 60.01, 0, "minecraft:cow", {}, false},
        {0, 20, 64, 0, "minecraft:sheep", {}, false},
        {0, 0, 68, 20, "minecraft:bee", {}, false},
        {0, 0, 59, -20, "minecraft:player", "Below", true}};
    auto layout = [&](double y, bool full = false, MapLayer layer = 0) {
        return layoutEntities(entities, {0,0,1,128,128}, {0,0,128,128}, layer, 0,y,0,s,locale,full);
    };
    auto markers = layout(64);
    require(markers.size() == 5, "Height fading preserves eligible entities");
    for (auto const& marker : markers) require(marker.iconScale == 0.75f, "Minimap portraits and fallback dots share the scale");
    require(markers[0].below && markers[0].opacity == 0.35f && markers[4].opacity == 0.35f && markers[4].name == "Below",
            "Entities and named players at least four blocks lower are faded");
    for (int i : {1, 2, 3}) require(!markers[i].below && markers[i].opacity == 1, "Near-level and higher entities remain opaque");
    auto full = layout(64, true);
    for (std::size_t i = 0; i < full.size(); ++i) require(full[i].iconScale == 1 && full[i].opacity == markers[i].opacity
        && full[i].anchorX == markers[i].anchorX && full[i].anchorY == markers[i].anchorY,
        "Fullscreen retains original icon size and relative height fading");
    s.minimapEntityScale = 2; s.entityPortraits = false;
    markers = layout(64);
    require(markers[0].iconScale == 2 && markers[0].opacity == 0.35f, "Fallback dots obey size and opacity preferences");
    markers = layout(60);
    require(std::all_of(markers.begin(), markers.end(), [](auto const& m) { return !m.below && m.opacity == 1; }),
            "Descending to the same floor immediately restores full opacity");
    s.entityDepthThreshold = 8;
    require(!layout(64)[0].below, "Changing the relative height threshold immediately updates a cached snapshot");
    s.entityDepthThreshold = 4; s.undergroundEntityOpacity = 1;
    require(layout(64)[0].opacity == 1, "Zero transparency disables fading without hiding lower entities");
    s.undergroundEntityOpacity = 0.1f;
    require(layout(64)[0].opacity == 0.1f, "Maximum transparency retains faint visible markers");
    for (auto& entity : entities) entity.y -= 100;
    markers = layout(-36, false, {0, -5});
    require(markers.size() == 5 && markers[0].below && !markers[2].below,
            "Underground maps use relative height even below Y zero");
    entities.front().y = -53;
    require(layout(-36, false, {0, -5}).size() == 4, "Fading does not bypass the cave layer's vertical limit");
}
void livingEntityTests() {
    require(livingRadarEntity(true, false, true, "minecraft:player"), "Living players do not require a mob category");
    for (auto type : {"minecraft:zombie", "minecraft:cow", "minecraft:iron_golem", "addon:guard"}) {
        require(livingRadarEntity(false, true, true, type), "Living vanilla and add-on mobs are accepted without a portrait");
        require(!livingRadarEntity(false, true, false, type), "Dead mobs are excluded");
    }
    require(!livingRadarEntity(true, true, false, "minecraft:player"), "Dead players are excluded");
    require(!livingRadarEntity(false, false, true, "addon:projectile"), "Unknown add-on non-mobs are excluded by runtime category");
    Settings s; Locale locale;
    std::vector<std::string> oldTypes{"minecraft:item", "minecraft:xp_orb", "minecraft:boat", "minecraft:chest_boat",
        "minecraft:minecart", "minecraft:arrow", "minecraft:tnt", "minecraft:armor_stand", "minecraft:tripod_camera"};
    for (auto const& type : oldTypes) {
        require(!livingRadarEntity(false, true, true, type), "Non-living special cases remain excluded even if classified as mobs");
        require(!allowedEntityType(s, type), "Show all types does not restore non-living markers");
    }
    s.entityFilterEnabled = true; s.entityTypes = oldTypes;
    auto choices = entityTypeChoices(oldTypes, s);
    for (auto const& type : oldTypes) require(std::find(choices.begin(), choices.end(), type) == choices.end()
        && !allowedEntityType(s, type), "Old saved and observed non-living selections cannot enter the menu or radar");
    std::vector<EntityMarker> stale{{0,0,64,0,"minecraft:item",{},false}, {0,0,64,0,"minecraft:armor_stand",{},false}};
    require(layoutEntities(stale,{0,0,1,128,128},{0,0,128,128},0,0,64,0,s,locale).empty(),
            "A stale non-living snapshot is also rejected by the render filter");
}
std::vector<PlacedEntity> crowdTests() {
    Settings s; Locale locale;
    MapView view{0,0,1,128,128}; MapRect area{0,0,128,128};
    std::vector<EntityMarker> mixed;
    for (int i = 0; i < 20; ++i) mixed.push_back({0,2,64,2,"minecraft:skeleton",{},false});
    for (auto type : {"minecraft:cow", "minecraft:pig", "minecraft:zombie", "minecraft:creeper", "minecraft:bee"})
        mixed.push_back({0,2,64,2,type,{},false});
    for (int i = 0; i < 6; ++i) mixed.push_back({0,2,56,2,"minecraft:skeleton",{},false});
    mixed.push_back({0,0,64,0,"minecraft:player","Alex",true});
    mixed.push_back({0,1,64,1,"minecraft:player","Steve",true});
    auto project = [&] { return layoutEntities(mixed,view,area,0,0,64,0,s,locale); };
    auto markers = project();
    require(markers.size() == 9, "Mixed crowded species, two height groups and both players keep separate markers");
    require(std::accumulate(markers.begin(), markers.end(), 0, [](int n, auto const& m) { return n + m.count; }) == 33,
            "Clustering preserves the exact total creature and player count");
    for (auto const& marker : markers) {
        auto box = entityVisualBounds(marker, s);
        require(box.x >= 0 && box.y >= 0 && box.x+box.width <= 128 && box.y+box.height <= 128,
                "Crowded portraits and count badges stay inside the map");
        require(!box.intersects({60,60,8,8}), "Crowded entities leave the local player's position visible");
        require(std::abs(marker.x-marker.anchorX) <= 37.5f && std::abs(marker.y-marker.anchorY) <= 37.5f,
                "Fan-out is bounded near the real position instead of moving icons across the map");
        if (marker.type == "minecraft:skeleton") require(marker.count == (marker.below ? 6 : 20)
            && marker.opacity == (marker.below ? 0.35f : 1.0f), "Surface and underground groups have independent counts and opacity");
        else require(marker.count == 1, "A common species never replaces another species or a player");
    }
    for (std::size_t i = 0; i < markers.size(); ++i) for (std::size_t j = i+1; j < markers.size(); ++j)
        require(!entityVisualBounds(markers[i],s).intersects(entityVisualBounds(markers[j],s)),
                "The mixed crowd's avatars and number badges do not obscure one another");
    for (auto const& player : markers) if (player.player)
        for (auto const& marker : markers) require(!player.label.intersects(entityVisualBounds(marker,s)),
                "Player names find space outside crowded heads and count badges");
    std::reverse(mixed.begin(), mixed.end());
    auto reversed = project();
    for (auto const& marker : markers) {
        auto other = std::find_if(reversed.begin(), reversed.end(), [&](auto const& m) {
            return m.type == marker.type && m.below == marker.below && m.name == marker.name;
        });
        require(other != reversed.end() && other->x == marker.x && other->y == marker.y && other->count == marker.count,
                "Changing engine iteration order does not reshuffle mixed species");
    }
    s.entityFilterEnabled = true; s.entityTypes = {"minecraft:cow"};
    auto filtered = project();
    require(filtered.size() == 1 && filtered[0].type == "minecraft:cow", "Decluttering respects explicit user type selections");
    s.entityFilterEnabled = false;
    mixed = {{0,1,64,1,"minecraft:cow",{},false}, {0,8,64,1,"minecraft:cow",{},false}};
    require(project().size() == 1, "Nearby same-species markers combine at normal minimap scale");
    view.blocksPerPixel = 0.25;
    require(project().size() == 2, "Zooming in naturally separates individual creatures again");
    view = {0,0,1,128,128};
    mixed.assign(512, {0,2,64,2,"minecraft:skeleton",{},false});
    auto crowded = project();
    require(crowded.size() == 1 && crowded[0].count == 512, "Dense same-species farms need only one portrait submission");
    mixed = {{0,-63,64,-63,"minecraft:cow",{},false}, {0,-62,64,-62,"minecraft:cow",{},false},
        {0,-63,64,-63,"minecraft:zombie",{},false}};
    for (auto const& marker : project()) {
        auto box = entityVisualBounds(marker,s);
        require(box.x >= 0 && box.y >= 0 && box.x+box.width <= 128 && box.y+box.height <= 128,
                "Edge clusters retain their complete count badges");
    }
    return markers;
}
void submissionTests() {
    // Model a UI batch that binds its last texture at flush. A mixed-species
    // submission would recolor every previous head as the final creature.
    std::vector<std::string_view> pending, rendered;
    std::string_view bound;
    int flushes = 0;
    for (auto type : {"minecraft:cow", "minecraft:zombie", "minecraft:creeper", "minecraft:skeleton"}) {
        auto portrait = entityPortrait(type);
        auto start = rendered.size();
        submitPortrait(*portrait, [&](std::string_view texture, PortraitPart const&) {
            bound = texture; pending.push_back(texture);
        }, [&] {
            for (auto texture : pending) {
                require(texture == bound, "A portrait submission never mixes different source textures");
                rendered.push_back(bound);
            }
            pending.clear(); ++flushes;
        });
        require(rendered.size() == start + portrait->parts.size() && rendered.back() == portrait->texture && pending.empty(),
                "Every creature's own head is submitted before the next source can overwrite texture state");
    }
    require(flushes == 4 && rendered.front() != rendered.back(), "A final skeleton cannot replace previously submitted cow faces");
}
} // namespace
int main(int argc, char** argv) {
    try {
        TestDirectory dir; settingsTests(dir); menuTests(); visibilityTests(); capacityTests(); portraitTests(); appearanceTests(); livingEntityTests();
        auto preview = crowdTests(); submissionTests();
        if (argc == 3 && std::string_view(argv[1]) == "--preview") {
            nlohmann::json document;
            for (auto const& m : preview) document["markers"].push_back({{"type",m.type},{"name",m.name},{"x",m.x},{"y",m.y},
                {"anchorX",m.anchorX},{"anchorY",m.anchorY},{"count",m.count},{"scale",m.iconScale},{"opacity",m.opacity},
                {"label",{m.label.x,m.label.y,m.label.width,m.label.height}}});
            Settings s;
            for (auto const& m : preview) {
                auto box = entityCountBounds(m,s);
                document["badges"].push_back({box.x,box.y,box.width,box.height});
            }
            for (auto const& p : entityPortraitCatalog) {
                auto& json = document["portraits"][p.type]; json["texture"] = p.texture;
                for (auto const& part : p.parts) json["parts"].push_back({part.u,part.v,part.uw,part.vh,part.x,part.y,part.width,part.height});
            }
            std::ofstream out(argv[2]); out << document.dump(2);
            if (!out) throw std::runtime_error("Unable to write requested radar preview");
        }
        std::cout << checks << " checks passed: entity filters, settings, menus, player names, projection and bounded snapshots.\n";
    } catch (std::exception const& ex) {
        std::cerr << "FAILED after " << checks << " checks: " << ex.what() << '\n'; return 1;
    }
}
