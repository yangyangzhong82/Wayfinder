#include "wayfinder/MapMenu.h"
#include "wayfinder/EntityRadar.h"
#include <charconv>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <set>

namespace wayfinder {
namespace {
std::string timestamp(std::int64_t seconds) {
    std::time_t t = static_cast<std::time_t>(seconds);
    std::tm     parts{};
    if (gmtime_s(&parts, &t) != 0) return "Unknown time";
    std::ostringstream out;
    out << std::put_time(&parts, "%m-%d %H:%M UTC");
    return out.str();
}
std::string keyName(int code) {
    if (code == 187) return "+";
    if (code == 189) return "-";
    if (code == 107) return "Num +";
    if (code == 109) return "Num -";
    if (code >= 0x70 && code <= 0x7b) return "F" + std::to_string(code - 0x6f);
    if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) return std::string(1, static_cast<char>(code));
    return "VK " + std::to_string(code);
}
} // namespace
void MapMenu::open(Page next) {
    page  = next;
    focus = UiAction::None;
    text.clear();
    message.clear();
    errorMessage  = false;
    contextLoading = false;
    offset        = 0;
    confirmDelete = false;
    confirmReset = false;
}
void MapMenu::edit(Waypoint point) {
    open(Page::Edit);
    draft = std::move(point);
}
void MapMenu::context(Waypoint point, float x, float y) {
    open(Page::Context);
    draft = std::move(point);
    contextX = x;
    contextY = y;
}
void MapMenu::scroll(int direction) {
    commitField();
    offset = std::clamp(offset + direction * visibleRows, 0, std::max(0, rowCount - visibleRows));
}
void MapMenu::beginField(UiAction field) {
    commitField();
    focus     = field;
    selectAll = true;
    if (field == UiAction::Name) text = draft.name;
    if (field == UiAction::Group) text = draft.group;
    if (field == UiAction::Search) text = query.search;
    if (field == UiAction::EntitySearch) text = entitySearch;
    if (field == UiAction::X) text = std::to_string(draft.x);
    if (field == UiAction::Z) text = std::to_string(draft.z);
    if (field == UiAction::Y) text = draft.y ? std::to_string(*draft.y) : "";
}
void MapMenu::commitField() {
    if (focus == UiAction::EntitySearch) {
        entitySearch = cleanName(text);
        offset = 0;
    } else if (focus == UiAction::Search) {
        query.search = cleanName(text);
        offset = 0;
    } else if (focus == UiAction::Group) {
        draft.group = cleanGroup(text);
    } else if (focus == UiAction::Name) {
        auto changed = draft;
        changed.name = cleanName(text);
        validateWaypoint(changed);
        draft = std::move(changed);
    } else if (focus == UiAction::X || focus == UiAction::Y || focus == UiAction::Z) {
        if (text.empty() && focus == UiAction::Y) draft.y.reset();
        else {
            int value{};
            auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
                throw std::runtime_error("Enter an integer coordinate");
            auto changed = draft;
            if (focus == UiAction::X) changed.x = value;
            if (focus == UiAction::Y) changed.y = value;
            if (focus == UiAction::Z) changed.z = value;
            validateWaypoint(changed);
            draft = std::move(changed);
        }
    }
    focus = UiAction::None;
    text.clear();
    selectAll = false;
}
void MapMenu::cancelField() {
    focus = UiAction::None;
    text.clear();
    selectAll = false;
}
void MapMenu::append(std::string const& input) {
    if (focus != UiAction::Name && focus != UiAction::Group && focus != UiAction::Search && focus != UiAction::EntitySearch
        && focus != UiAction::X && focus != UiAction::Y && focus != UiAction::Z) return;
    std::string next = selectAll ? "" : text;
    if (focus == UiAction::Name || focus == UiAction::Group || focus == UiAction::Search || focus == UiAction::EntitySearch) next = cleanName(next + input);
    else {
        auto first = input.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return;
        auto last = input.find_last_not_of(" \t\r\n");
        for (char ch : std::string_view(input).substr(first, last - first + 1)) {
            if ((ch >= '0' && ch <= '9') || (ch == '-' && next.empty())) next += ch;
            else throw std::runtime_error("Enter an integer coordinate");
        }
        if (next.size() > 10) throw std::runtime_error("Coordinates outside world bounds");
    }
    text      = std::move(next);
    selectAll = false;
}
void MapMenu::backspace() {
    if (selectAll) text.clear();
    else eraseLastCharacter(text);
    selectAll = false;
}
int MapMenu::action(UiButton const& b, Settings& s, Navigation& nav) {
    if (!b.enabled || b.action == UiAction::None) return 0;
    message.clear();
    errorMessage = false;
    if (b.action != UiAction::ResetSettings) confirmReset = false;
    if (b.action == UiAction::SettingsTab) {
        cancelField();
        settingsTab = std::clamp(static_cast<int>(b.id), 0, 2);
        offset = 0;
        return 0;
    }
    if (b.action != UiAction::Delete) confirmDelete = false;
    if (b.action == UiAction::Back) {
        open(page == Page::Entities ? Page::Settings : Page::Map);
        return 0;
    }
    if (b.action == UiAction::Previous || b.action == UiAction::Next) {
        scroll(b.action == UiAction::Next ? 1 : -1);
        return 0;
    }
    if (b.action == UiAction::Name || b.action == UiAction::Group || b.action == UiAction::Search || b.action == UiAction::EntitySearch
        || b.action == UiAction::X || b.action == UiAction::Y || b.action == UiAction::Z) {
        beginField(b.action);
        return 0;
    }
    commitField();
    switch (b.action) {
    case UiAction::Entities:
        open(Page::Entities);
        return 0;
    case UiAction::ShowEntities:
        s.showEntities = !s.showEntities;
        break;
    case UiAction::EntityPortraits:
        s.entityPortraits = !s.entityPortraits;
        break;
    case UiAction::EntityScaleDown:
        s.minimapEntityScale -= 0.05f;
        break;
    case UiAction::EntityScaleUp:
        s.minimapEntityScale += 0.05f;
        break;
    case UiAction::EntityTransparencyDown:
        s.undergroundEntityOpacity += 0.05f;
        break;
    case UiAction::EntityTransparencyUp:
        s.undergroundEntityOpacity -= 0.05f;
        break;
    case UiAction::EntityDepthDown:
        --s.entityDepthThreshold;
        break;
    case UiAction::EntityDepthUp:
        ++s.entityDepthThreshold;
        break;
    case UiAction::EntityFilterMode:
        s.entityFilterEnabled = !s.entityFilterEnabled;
        break;
    case UiAction::EntityPlayersOnly:
        s.entityFilterEnabled = true;
        s.entityTypes = {"minecraft:player"};
        break;
    case UiAction::EntityAll:
        s.entityFilterEnabled = false;
        break;
    case UiAction::EntityNone:
        s.entityFilterEnabled = true;
        s.entityTypes.clear();
        break;
    case UiAction::EntityType: {
        auto type = entityTypeId(b.data);
        auto it = std::find(s.entityTypes.begin(), s.entityTypes.end(), type);
        if (it == s.entityTypes.end()) s.entityTypes.push_back(std::move(type));
        else s.entityTypes.erase(it);
        s.entityFilterEnabled = true;
        break;
    }
    case UiAction::EntityRadiusDown:
        s.entityRadius -= 16;
        break;
    case UiAction::EntityRadiusUp:
        s.entityRadius += 16;
        break;
    case UiAction::ContextWaypoint:
        if (page == Page::Context) edit(draft);
        return 0;
    case UiAction::DimensionFilter: {
        std::set<int> dimensions{query.playerDimension};
        for (auto const& point : nav.points) dimensions.insert(point.dimension);
        auto next = query.dimension ? dimensions.upper_bound(*query.dimension) : dimensions.begin();
        query.dimension = next == dimensions.end() ? std::nullopt : std::optional<int>(*next);
        offset = 0;
        return 0;
    }
    case UiAction::GroupFilter: {
        std::set<std::string> groups{""};
        for (auto const& point : nav.points) groups.insert(point.group);
        auto next = query.group ? groups.upper_bound(*query.group) : groups.begin();
        query.group = next == groups.end() ? std::nullopt : std::optional<std::string>(*next);
        offset = 0;
        return 0;
    }
    case UiAction::SortWaypoints:
        query.sort = static_cast<WaypointQuery::Sort>((static_cast<int>(query.sort) + 1) % 3);
        offset = 0;
        return 0;
    case UiAction::ClearFilters:
        query.search.clear();
        query.dimension.reset();
        query.group.reset();
        query.sort = WaypointQuery::Sort::Recent;
        offset = 0;
        return 0;
    case UiAction::NavigateExisting:
        if (!nav.find(b.id)) return 0;
        nav.target = b.id;
        open(Page::Map);
        return 1;
    case UiAction::MinimapZoomIn:
    case UiAction::MinimapZoomOut:
        if (!zoomMinimap(s, b.action == UiAction::MinimapZoomIn)) return 0;
        break;
    case UiAction::Water:
        s.includeWater = !s.includeWater;
        break;
    case UiAction::TerrainMode:
        s.terrainMode = s.terrainMode == "auto" ? "surface" : s.terrainMode == "surface" ? "cave" : "auto";
        break;
    case UiAction::CaveHeightDown:
        if (s.caveSwitchY <= -64) return 0;
        --s.caveSwitchY;
        break;
    case UiAction::CaveHeightUp:
        if (s.caveSwitchY >= 320) return 0;
        ++s.caveSwitchY;
        break;
    case UiAction::Leaves:
        s.includeLeaves = !s.includeLeaves;
        break;
    case UiAction::Performance: {
        auto preset = performancePreset(s);
        applyPerformancePreset(s, preset == PerformancePreset::Custom ? PerformancePreset::Balanced
            : static_cast<PerformancePreset>((static_cast<int>(preset) + 1) % 3));
        break;
    }
    case UiAction::ResetSettings:
        if (!confirmReset) {
            confirmReset = true;
            message = "Click again to reset; history, waypoints and profile are kept";
            return 0;
        }
        resetMapPreferences(s);
        confirmReset = false;
        break;
    case UiAction::LocateJump:
        validateWaypoint(draft);
        return 3;
    case UiAction::LocateWaypoint:
        validateWaypoint(draft);
        return 4;
    case UiAction::Scale:
        s.showScale = !s.showScale;
        break;
    case UiAction::Compass:
        s.showCompass = !s.showCompass;
        break;
    case UiAction::ChunkBorders:
        s.showChunkBorders = !s.showChunkBorders;
        break;
    case UiAction::Biome:
        s.showBiome = !s.showBiome;
        break;
    case UiAction::Language:
        s.language = s.language == "auto" ? "zh_CN" : s.language == "zh_CN" ? "en_US" : "auto";
        break;
    case UiAction::List:
        open(Page::List);
        return 0;
    case UiAction::Settings:
        open(Page::Settings);
        return 0;
    case UiAction::OpenWaypoint:
        if (auto p = nav.find(b.id)) edit(*p);
        return 0;
    case UiAction::Color:
        draft.color = (draft.color + 1) % markerColors.size();
        return 0;
    case UiAction::Icon:
        draft.icon = (draft.icon + 1) % markerIcons.size();
        return 0;
    case UiAction::Keep:
        draft.death = false;
        message     = "Save to keep this death point permanently";
        return 0;
    case UiAction::Save:
    case UiAction::Navigate: {
        auto id = nav.save(draft);
        if (b.action == UiAction::Navigate) nav.target = id;
        open(Page::List);
        return 1;
    }
    case UiAction::Delete:
        if (!draft.id) {
            open(Page::List);
            return 0;
        }
        if (!confirmDelete) {
            confirmDelete = true;
            message       = "Click Delete again to remove this waypoint";
            return 0;
        }
        nav.remove(draft.id);
        open(Page::List);
        return 1;
    case UiAction::Stop:
        nav.target = 0;
        return 1;
    case UiAction::MapKey:
        focus   = b.action;
        message = "Press a letter, number or F1-F12; Esc cancels";
        return 0;
    case UiAction::ToggleKey:
    case UiAction::ZoomInKey:
    case UiAction::ZoomOutKey:
        focus   = b.action;
        message = b.action == UiAction::ToggleKey ? "Press a letter, number or F1-F12; Esc cancels"
                                                  : "Press a zoom key (+/- allowed); Esc cancels";
        return 0;
    case UiAction::ShowMap:
        s.showMinimap = !s.showMinimap;
        break;
    case UiAction::SizeDown:
        s.minimapSize -= 8;
        break;
    case UiAction::SizeUp:
        s.minimapSize += 8;
        break;
    case UiAction::OpacityDown:
        s.minimapOpacity -= 0.1f;
        break;
    case UiAction::OpacityUp:
        s.minimapOpacity += 0.1f;
        break;
    case UiAction::Left:
        s.minimapPositionX -= 0.1f;
        break;
    case UiAction::Right:
        s.minimapPositionX += 0.1f;
        break;
    case UiAction::Up:
        s.minimapPositionY -= 0.1f;
        break;
    case UiAction::Down:
        s.minimapPositionY += 0.1f;
        break;
    case UiAction::Coordinates:
        s.showCoordinates = !s.showCoordinates;
        break;
    case UiAction::Waypoints:
        s.showWaypoints = !s.showWaypoints;
        break;
    case UiAction::Navigation:
        s.showNavigation = !s.showNavigation;
        break;
    case UiAction::Deaths:
        s.recordDeaths = !s.recordDeaths;
        break;
    default:
        return 0;
    }
    normalizeSettings(s);
    message = "Saved - applies immediately";
    return 2;
}
UiFrame MapMenu::build(float width, float height, Settings const& s, Navigation const& nav) {
    UiFrame frame;
    auto    button = [&](MapRect r, std::string text, UiAction action, std::uint64_t id = 0, bool selected = false) {
        frame.buttons.push_back({r, std::move(text), action, id, selected});
    };
    if (page == Page::Context) {
        frame.modal = true;
        frame.backdrop = {0, 0, width, height};
        float w = std::min(260.0f, width - 8), h = 128;
        float x = std::clamp(contextX, 4.0f, std::max(4.0f, width - w - 4));
        float y = std::clamp(contextY, 4.0f, std::max(4.0f, height - h - 4));
        frame.panel = {x, y, w, h};
        frame.title = locale.tr("Selected location");
        frame.message = errorMessage ? locale.error(message) : locale.tr(message);
        if (frame.message.empty()) frame.message = locale.tr("Teleport requires command permission.");
        std::string coordinates = "X: " + std::to_string(draft.x) + "  Y: "
            + (draft.y ? std::to_string(*draft.y) : "?") + "  Z: " + std::to_string(draft.z);
        button({x + 6, y + 25, w - 12, 18}, coordinates, UiAction::None);
        button({x + 6, y + 47, w - 12, 18}, locale.tr("Teleport"), UiAction::Teleport);
        frame.buttons.back().enabled = draft.y.has_value();
        button({x + 6, y + 69, w - 12, 18}, locale.tr("Create waypoint here"), UiAction::ContextWaypoint);
        button({x + 6, y + 91, w - 12, 18}, locale.tr("Back / Cancel"), UiAction::Back);
        if (!draft.y && !errorMessage) frame.message = locale.tr(contextLoading
            ? "Loading terrain height..." : "Unknown height; teleport unavailable here.");
        return frame;
    }
    if (page == Page::Map) {
        bool wide = width >= 360;
        float w = wide ? std::min(66.0f, (width - 36) / 5) : (width - 28) / 3;
        float start = wide ? width - 10 - (w * 5 + 16) : 10;
        button({start, 8, w, 18}, locale.tr("+ Here"), UiAction::NewPlayer);
        button({start + w + 4, 8, w, 18}, locale.tr("Waypoints"), UiAction::List);
        button({start + (w + 4) * 2, 8, w, 18}, locale.tr("Settings"), UiAction::Settings);
        float secondWidth = wide ? w : (width - 24) / 2;
        button({wide ? start + (w + 4) * 3 : 10, wide ? 8.0f : 28.0f, secondWidth, 18},
               locale.tr("Locate"), UiAction::Locate);
        button({wide ? start + (w + 4) * 4 : 14 + secondWidth, wide ? 8.0f : 28.0f, secondWidth, 18},
               locale.tr("Follow"), UiAction::Follow);
        return frame;
    }
    frame.modal      = true;
    float panelWidth = std::min(300.0f, width - 16), panelHeight = height - 16;
    frame.backdrop = {0, 0, width, height};
    frame.panel   = {(width - panelWidth) / 2, 8, panelWidth, panelHeight};
    frame.title   = page == Page::List   ? locale.tr("Waypoints")
                  : page == Page::Entities ? locale.tr("Entity display")
                  : page == Page::Edit   ? locale.tr("EDIT WAYPOINT")
                  : page == Page::Locate ? locale.tr("LOCATE COORDINATES")
                                         : locale.tr("MAP SETTINGS");
    frame.message = errorMessage ? locale.error(message) : locale.tr(message);
    if (frame.message.empty() && (page == Page::Edit || (page == Page::List && focus == UiAction::Search)
        || (page == Page::Entities && focus == UiAction::EntitySearch)))
        frame.message = focus == UiAction::None ? locale.tr("Click a field to edit. Esc cancels draft.")
                                                : locale.tr("Type / Ctrl+V paste | Enter: done | Esc: cancel");
    std::vector<std::vector<UiButton>> rows;
    auto                               row = [&](std::string label, UiAction action, std::uint64_t id = 0) {
        rows.push_back({
            {{}, std::move(label), action, id, focus == action && focus != UiAction::None}
        });
    };
    auto pair = [&](std::string a, UiAction aa, std::string b, UiAction ba) {
        rows.push_back({
            {{}, std::move(a), aa},
            {{}, std::move(b), ba}
        });
    };
    auto setting = [&](std::string key, UiAction action, std::string value, bool toggle = false, bool selected = false) {
        row(locale.tr(key), action);
        auto& b = rows.back().front();
        b.value = std::move(value);
        b.toggle = toggle;
        b.selected = toggle ? selected : focus == action;
    };
    auto toggle = [&](std::string key, UiAction action, bool value) {
        setting(key, action, locale.tr(value ? "ON" : "OFF"), true, value);
    };
    auto stepper = [&](std::string key, std::string value, UiAction down, UiAction up) {
        rows.push_back({{{}, locale.tr(key), UiAction::None}, {{}, "-", down}, {{}, "+", up}});
        rows.back().front().value = std::move(value);
    };
    if (page == Page::Entities) {
        toggle("Show nearby entities", UiAction::ShowEntities, s.showEntities);
        toggle("Entity portraits", UiAction::EntityPortraits, s.entityPortraits);
        stepper("Minimap entity scale", std::to_string(std::lround(s.minimapEntityScale * 100)) + "%",
            UiAction::EntityScaleDown, UiAction::EntityScaleUp);
        stepper("Lower entity transparency", std::to_string(std::lround((1 - s.undergroundEntityOpacity) * 100)) + "%",
            UiAction::EntityTransparencyDown, UiAction::EntityTransparencyUp);
        stepper("Lower entity height difference", std::to_string(s.entityDepthThreshold) + locale.tr(" blocks"),
            UiAction::EntityDepthDown, UiAction::EntityDepthUp);
        setting("Entity filter", UiAction::EntityFilterMode, locale.tr(s.entityFilterEnabled ? "Selected types only" : "All types"));
        stepper("Entity radius", std::to_string(s.entityRadius) + locale.tr(" blocks"), UiAction::EntityRadiusDown, UiAction::EntityRadiusUp);
        pair(locale.tr("Players only"), UiAction::EntityPlayersOnly, locale.tr("Show all types"), UiAction::EntityAll);
        row(locale.tr("Clear entity selection"), UiAction::EntityNone);
        row(locale.tr("Search: ") + (focus == UiAction::EntitySearch ? "[" + text + "_]"
            : entitySearch.empty() ? locale.tr("All types") : entitySearch), UiAction::EntitySearch);
        auto choices = entityTypeChoices(observedEntityTypes, s);
        for (auto const& type : choices) {
            if (!entityTypeMatches(type, entitySearch, locale)) continue;
            toggle(entityTypeLabel(type, locale), UiAction::EntityType, selectedEntityType(s, type));
            rows.back().front().data = type;
        }
        if (frame.message.empty()) frame.message = locale.tr("Entities loaded: ") + std::to_string(loadedEntities)
            + locale.tr(" | Nearby: ") + std::to_string(nearbyEntities)
            + locale.tr(" | Matched: ") + std::to_string(visibleEntities);
    } else if (page == Page::List) {
        row(locale.tr("Search: ") + (focus == UiAction::Search ? "[" + text + "_]"
            : query.search.empty() ? locale.tr("All names") : query.search), UiAction::Search);
        pair(locale.tr("Dimension: ") + (query.dimension ? dimensionName(*query.dimension, locale) : locale.tr("All")),
             UiAction::DimensionFilter, locale.tr("Sort: ") + locale.tr(query.sort == WaypointQuery::Sort::Recent
                 ? "Newest" : query.sort == WaypointQuery::Sort::Distance ? "Distance" : "Name"), UiAction::SortWaypoints);
        pair(locale.tr("Group: ") + (!query.group ? locale.tr("All") : query.group->empty() ? locale.tr("Ungrouped") : *query.group),
             UiAction::GroupFilter, locale.tr("Clear filters"), UiAction::ClearFilters);
        pair(locale.tr("+ At player"), UiAction::NewPlayer, locale.tr("Stop navigation"), UiAction::Stop);
        auto points = queryWaypoints(nav, query, locale);
        if (points.empty()) row(locale.tr(nav.points.empty() ? "No waypoints yet" : "No matching waypoints"), UiAction::None);
        for (auto point : points) {
            auto detail = dimensionName(point->dimension, locale);
            if (point->dimension == query.playerDimension)
                detail += " | " + std::to_string(static_cast<int>(std::round(std::hypot(
                    point->x + 0.5 - query.playerX, point->z + 0.5 - query.playerZ)))) + locale.tr("m");
            row((nav.target == point->id ? "> " : "") + waypointName(*point, locale)
                + (point->group.empty() ? "" : " [" + point->group + "]") + " | " + detail
                + (point->death ? " | " + timestamp(point->created) : ""), UiAction::OpenWaypoint, point->id);
        }
    } else if (page == Page::Edit) {
        auto field = [&](UiAction action, std::string value) { return focus == action ? "[" + text + "_]" : value; };
        row(locale.tr("Name: ") + field(UiAction::Name, waypointName(draft, locale)), UiAction::Name);
        row(locale.tr("Group: ") + field(UiAction::Group, draft.group.empty() ? locale.tr("Ungrouped") : draft.group), UiAction::Group);
        row("X: " + field(UiAction::X, std::to_string(draft.x)), UiAction::X);
        row("Y: " + field(UiAction::Y, draft.y ? std::to_string(*draft.y) : locale.tr("Unknown (optional)")),
            UiAction::Y);
        row("Z: " + field(UiAction::Z, std::to_string(draft.z)), UiAction::Z);
        static char const* colors[]{"Yellow", "Blue", "Green", "Red", "Purple", "Orange", "Pink", "White"};
        pair(
            std::string(locale.tr("Color: ")) + locale.tr(colors[draft.color]),
            UiAction::Color,
            std::string(locale.tr("Icon: ")) + locale.tr(markerIcons[draft.icon]),
            UiAction::Icon
        );
        row(dimensionName(draft.dimension, locale) + " | " + locale.tr(timestamp(draft.created)), UiAction::None);
        if (draft.death) row(locale.tr("Convert to permanent waypoint"), UiAction::Keep);
        pair(locale.tr("Save"), UiAction::Save, locale.tr("Save + Navigate"), UiAction::Navigate);
        pair(
            confirmDelete ? locale.tr("Confirm Delete") : locale.tr("Delete"),
            UiAction::Delete,
            locale.tr("Stop navigation"),
            UiAction::Stop
        );
    } else if (page == Page::Locate) {
        auto field = [&](UiAction action, int value) {
            return focus == action ? "[" + text + "_]" : std::to_string(value);
        };
        row("X: " + field(UiAction::X, draft.x), UiAction::X);
        row("Z: " + field(UiAction::Z, draft.z), UiAction::Z);
        row(dimensionName(draft.dimension, locale), UiAction::None);
        row(locale.tr("Jump to coordinates"), UiAction::LocateJump);
        row(locale.tr("Create waypoint here"), UiAction::LocateWaypoint);
        if (frame.message.empty()) frame.message = locale.tr("View only; does not teleport or load chunks.");
    } else if (settingsTab == 0) {
        toggle("Minimap", UiAction::ShowMap, s.showMinimap);
        stepper("Map size", std::to_string(int(s.minimapSize)), UiAction::SizeDown, UiAction::SizeUp);
        std::ostringstream zoom;
        zoom << std::fixed << std::setprecision(2) << (2.0 / s.minimapBlocksPerPixel) << "x";
        stepper("Minimap zoom", zoom.str(), UiAction::MinimapZoomOut, UiAction::MinimapZoomIn);
        stepper("Opacity", std::to_string(int(std::round(s.minimapOpacity * 100))) + "%", UiAction::OpacityDown, UiAction::OpacityUp);
        stepper("Horizontal position", std::to_string(int(std::round(s.minimapPositionX * 100))) + "%", UiAction::Left, UiAction::Right);
        stepper("Vertical position", std::to_string(int(std::round(s.minimapPositionY * 100))) + "%", UiAction::Up, UiAction::Down);
        if (frame.message.empty()) frame.message = locale.tr("Position: left to right / top to bottom");
    } else if (settingsTab == 1) {
        toggle("Show nearby entities", UiAction::ShowEntities, s.showEntities);
        row(locale.tr("Entity display / filter"), UiAction::Entities);
        setting("Terrain mode", UiAction::TerrainMode, locale.tr("terrain." + s.terrainMode));
        stepper("Cave switch height", "Y " + std::to_string(s.caveSwitchY),
                UiAction::CaveHeightDown, UiAction::CaveHeightUp);
        toggle("Coordinates", UiAction::Coordinates, s.showCoordinates);
        toggle("Show biome", UiAction::Biome, s.showBiome);
        toggle("Show scale", UiAction::Scale, s.showScale);
        toggle("Compass", UiAction::Compass, s.showCompass);
        toggle("Chunk borders", UiAction::ChunkBorders, s.showChunkBorders);
        toggle("Waypoints", UiAction::Waypoints, s.showWaypoints);
        toggle("Target guidance", UiAction::Navigation, s.showNavigation);
        toggle("Include water", UiAction::Water, s.includeWater);
        toggle("Include leaves", UiAction::Leaves, s.includeLeaves);
    } else {
        setting("Language", UiAction::Language, locale.tr(s.language == "auto" ? "Follow game" : "language." + s.language));
        toggle("Record deaths", UiAction::Deaths, s.recordDeaths);
        setting("Full map key", UiAction::MapKey, keyName(s.fullMapKey));
        setting("Minimap toggle key", UiAction::ToggleKey, keyName(s.toggleMinimapKey));
        setting("Minimap zoom in key", UiAction::ZoomInKey, keyName(s.minimapZoomInKey));
        setting("Minimap zoom out key", UiAction::ZoomOutKey, keyName(s.minimapZoomOutKey));
        static char const* presets[]{"Light", "Balanced", "Quality", "Custom"};
        setting("Performance preset", UiAction::Performance, locale.tr(presets[static_cast<int>(performancePreset(s))]));
        row(locale.tr(confirmReset ? "Confirm reset" : "Restore defaults"), UiAction::ResetSettings);
    }
    if (frame.message.empty() && page == Page::Settings) frame.message = locale.tr("Saved - applies immediately");
    rowCount = static_cast<int>(rows.size());
    float contentTop = page == Page::Settings ? 46.0f : 26.0f;
    float rowStep = 22, rowHeight = 20;
    panelHeight = std::min(panelHeight, contentTop + rowCount * rowStep + 40);
    frame.panel = {(width - panelWidth) / 2, (height - panelHeight) / 2, panelWidth, panelHeight};
    visibleRows = std::max(1, static_cast<int>((panelHeight - contentTop - 40) / rowStep));
    int pages = (rowCount + visibleRows - 1) / visibleRows;
    offset = std::clamp(offset, 0, std::max(0, rowCount - visibleRows));
    if (page == Page::Settings) {
        static char const* tabs[]{"Appearance", "Map layers", "General"};
        float w = (panelWidth - 20) / 3;
        for (int i = 0; i < 3; ++i)
            button({frame.panel.x + 6 + i * (w + 4), frame.panel.y + 23, w, 18},
                   locale.tr(tabs[i]), UiAction::SettingsTab, i, settingsTab == i);
    }
    for (int i = offset; i < std::min(rowCount, offset + visibleRows); ++i) {
        auto const& group = rows[i];
        float w = (panelWidth - 12 - 4 * (group.size() - 1)) / group.size();
        float x = frame.panel.x + 6, y = frame.panel.y + contentTop + (i - offset) * rowStep;
        for (std::size_t j = 0; j < group.size(); ++j) {
            auto b = group[j];
            // Keep labels and values together, with two compact adjustment buttons.
            float cellWidth = group.size() == 3 ? (j == 0 ? panelWidth - 60 : 20) : w;
            b.rect = {x, y, cellWidth, rowHeight};
            frame.buttons.push_back(std::move(b));
            x += cellWidth + 4;
        }
    }
    float bottom = frame.panel.y + panelHeight - 36;
    float w = (panelWidth - 20) / 3;
    button({frame.panel.x + 6, bottom, w, 18}, locale.tr("Back / Cancel"), UiAction::Back);
    if (pages > 1) {
        button({frame.panel.x + 10 + w, bottom, w, 18}, locale.tr("Previous"), UiAction::Previous);
        frame.buttons.back().enabled = offset > 0;
        button({frame.panel.x + 14 + w * 2, bottom, w, 18}, locale.tr("Next"), UiAction::Next);
        frame.buttons.back().enabled = offset + visibleRows < rowCount;
        frame.pagination = std::to_string((offset + visibleRows - 1) / visibleRows + 1) + " / " + std::to_string(pages);
    }
    return frame;
}
} // namespace wayfinder
