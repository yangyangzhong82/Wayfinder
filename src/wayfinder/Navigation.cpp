#include "wayfinder/Navigation.h"
#include <Windows.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace wayfinder {
std::string cleanName(std::string const& text) {
    // Decode UTF-8 strictly. Reject formatting codes/control characters and cap by
    // code points rather than truncating a Chinese name inside a byte sequence.
    std::string result;
    unsigned    count{};
    for (std::size_t i = 0; i < text.size();) {
        auto        lead   = static_cast<unsigned char>(text[i]);
        std::size_t length = lead < 0x80                  ? 1
                           : lead >= 0xc2 && lead <= 0xdf ? 2
                           : lead >= 0xe0 && lead <= 0xef ? 3
                           : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                          : 0;
        if (!length || i + length > text.size()) throw std::runtime_error("Invalid UTF-8 name");
        unsigned cp = lead & (length == 1 ? 0x7f : (1u << (7 - length)) - 1);
        for (std::size_t j = 1; j < length; ++j) {
            auto next = static_cast<unsigned char>(text[i + j]);
            if ((next & 0xc0) != 0x80) throw std::runtime_error("Invalid UTF-8 name");
            cp = (cp << 6) | (next & 0x3f);
        }
        if ((length == 2 && cp < 0x80) || (length == 3 && cp < 0x800) || (length == 4 && cp < 0x10000) || cp > 0x10ffff
            || (cp >= 0xd800 && cp <= 0xdfff))
            throw std::runtime_error("Invalid UTF-8 name");
        if (cp >= 32 && cp != 127 && cp != 0xa7 && !(cp >= 0x80 && cp <= 0x9f)) {
            if (++count > 32) break;
            result.append(text, i, length);
        }
        i += length;
    }
    return result;
}
void eraseLastCharacter(std::string& text) {
    if (text.empty()) return;
    auto index = text.size() - 1;
    while (index && (static_cast<unsigned char>(text[index]) & 0xc0) == 0x80) --index;
    text.resize(index);
}
std::string cleanGroup(std::string const& text) {
    auto result = cleanName(text);
    auto first = result.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return result.substr(first, result.find_last_not_of(' ') - first + 1);
}
void validateWaypoint(Waypoint const& p) {
    if (p.name.empty() || cleanName(p.name) != p.name || p.name.find_first_not_of(' ') == std::string::npos)
        throw std::runtime_error("Enter a name (1-32 characters)");
    if (cleanGroup(p.group) != p.group) throw std::runtime_error("Invalid waypoint group");
    if (p.x < -29999984 || p.x > 29999984 || p.z < -29999984 || p.z > 29999984
        || (p.y && (*p.y < -32768 || *p.y > 32767)))
        throw std::runtime_error("Coordinates outside world bounds");
    if (p.color < 0 || p.color >= static_cast<int>(markerColors.size()) || p.icon < 0 || p.icon >= 4 || p.created < 0)
        throw std::runtime_error("Invalid waypoint appearance/time");
}
std::string dimensionName(int dimension, Locale const& locale) {
    if (dimension == 0) return locale.tr("Overworld");
    if (dimension == 1) return locale.tr("Nether");
    if (dimension == 2) return locale.tr("End");
    return locale.tr("Dimension ") + std::to_string(dimension);
}
std::string waypointName(Waypoint const& point, Locale const& locale) {
    // Only the automatic death label is translated; custom names remain verbatim.
    return point.death && point.name == "Death" ? locale.tr("Death") : point.name;
}
Waypoint const* Navigation::find(std::uint64_t id) const {
    if (!id) return nullptr;
    auto it = std::find_if(points.begin(), points.end(), [&](auto const& p) { return p.id == id; });
    return it == points.end() ? nullptr : &*it;
}
bool Navigation::groupVisible(std::string const& group, bool fullscreen) const {
    auto const& hidden = fullscreen ? hiddenFullMapGroups : hiddenMinimapGroups;
    return std::find(hidden.begin(), hidden.end(), group) == hidden.end();
}
void Navigation::toggleGroup(std::string const& group, bool fullscreen) {
    if (cleanGroup(group) != group) throw std::runtime_error("Invalid waypoint group");
    auto& hidden = fullscreen ? hiddenFullMapGroups : hiddenMinimapGroups;
    auto found = std::find(hidden.begin(), hidden.end(), group);
    if (found != hidden.end()) hidden.erase(found);
    else {
        if (hidden.size() >= 1024) throw std::runtime_error("Too many hidden waypoint groups");
        hidden.push_back(group);
    }
}
std::vector<Waypoint const*> queryWaypoints(Navigation const& nav, WaypointQuery const& query, Locale const& locale) {
    auto fold = [](std::string text) {
        for (auto& ch : text) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
        return text;
    };
    auto search = fold(query.search);
    std::vector<Waypoint const*> result;
    for (auto const& point : nav.points) {
        if (query.dimension && point.dimension != *query.dimension) continue;
        if (query.group && point.group != *query.group) continue;
        if (!search.empty() && fold(waypointName(point, locale)).find(search) == std::string::npos) continue;
        result.push_back(&point);
    }
    std::sort(result.begin(), result.end(), [&](auto a, auto b) {
        if (a->favorite != b->favorite) return a->favorite;
        if (query.sort == WaypointQuery::Sort::Distance) {
            bool ac = a->dimension == query.playerDimension, bc = b->dimension == query.playerDimension;
            if (ac != bc) return ac; // Never compare distances across dimensions.
            if (ac) {
                double ad = std::hypot(a->x + 0.5 - query.playerX, a->z + 0.5 - query.playerZ);
                double bd = std::hypot(b->x + 0.5 - query.playerX, b->z + 0.5 - query.playerZ);
                if (ad != bd) return ad < bd;
            }
        } else if (query.sort == WaypointQuery::Sort::Name) {
            auto an = fold(waypointName(*a, locale)), bn = fold(waypointName(*b, locale));
            if (an != bn) return an < bn;
        }
        if (a->created != b->created) return a->created > b->created;
        return a->id > b->id;
    });
    return result;
}
std::uint64_t Navigation::save(Waypoint point) {
    validateWaypoint(point);
    auto previous = find(point.id);
    if (!point.death && (!previous || previous->death)
        && std::count_if(points.begin(), points.end(), [](auto const& p) { return !p.death; }) >= 512)
        throw std::runtime_error("Permanent waypoint limit reached (512)");
    if (point.id) {
        auto it = std::find_if(points.begin(), points.end(), [&](auto const& p) { return p.id == point.id; });
        if (it == points.end()) throw std::runtime_error("Waypoint no longer exists");
        *it = std::move(point);
        return it->id;
    }
    if (points.size() >= 517) throw std::runtime_error("Waypoint limit reached (517)");
    if (nextId == 0 || nextId == std::numeric_limits<std::uint64_t>::max())
        throw std::runtime_error("Waypoint ID limit");
    point.id = nextId++;
    points.push_back(std::move(point));
    return points.back().id;
}
void Navigation::remove(std::uint64_t id) {
    std::erase_if(points, [&](auto const& p) { return p.id == id; });
    if (target == id) target = 0;
}
void Navigation::recordDeath(int dimension, double x, double y, double z, std::int64_t time) {
    auto deaths = [&] { return std::count_if(points.begin(), points.end(), [](auto const& p) { return p.death; }); };
    while (deaths() >= 5) {
        auto it = std::find_if(points.begin(), points.end(), [](auto const& p) { return p.death; });
        remove(it->id);
    }
    Waypoint p;
    p.name      = "Death";
    p.dimension = dimension;
    p.x         = blockCoordinate(x);
    p.y         = blockCoordinate(y);
    p.z         = blockCoordinate(z);
    p.color     = 3;
    p.icon      = 3;
    p.death     = true;
    p.created   = time;
    save(std::move(p));
}
MarkerProjection projectMarker(MapView const& view, double worldX, double worldZ, double width, double height) {
    auto   origin = view.worldAt(0, 0);
    double x      = (worldX - origin[0]) / (view.width * view.blocksPerPixel) * width;
    double y      = (worldZ - origin[1]) / (view.height * view.blocksPerPixel) * height;
    double dx = x - width / 2, dy = y - height / 2;
    double halfX = std::max(1.0, width / 2 - 7), halfY = std::max(1.0, height / 2 - 7);
    double factor = std::max({1.0, std::abs(dx) / halfX, std::abs(dy) / halfY});
    return {width / 2 + dx / factor, height / 2 + dy / factor, factor > 1, dx, dy};
}
std::string targetDescription(Waypoint const& p, int dimension, double x, double y, double z, Locale const& locale) {
    if (p.dimension != dimension) return dimensionName(p.dimension, locale) + " | " + waypointName(p, locale);
    double dx = p.x + 0.5 - x, dz = p.z + 0.5 - z;
    double distance = std::hypot(dx, dz);
    double height = p.y ? *p.y - y : 0;
    if (distance < 2 && p.y && std::abs(height) < 2)
        return locale.tr("Arrived") + " | " + waypointName(p, locale);
    static char const* directions[]{"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    auto               sector = static_cast<int>(std::floor(std::atan2(dx, -dz) / (3.141592653589793 / 4) + 0.5));
    auto vertical = !p.y ? locale.tr("Height unknown")
        : std::abs(height) < 2 ? locale.tr("Same height")
        : locale.tr(height > 0 ? "Above: " : "Below: ")
            + std::to_string(static_cast<int>(std::round(std::abs(height)))) + locale.tr("m");
    return std::to_string(static_cast<int>(std::round(distance))) + locale.tr("m ")
         + (distance < 2 ? locale.tr("At X/Z") : locale.tr(directions[(sector + 8) % 8]))
         + " | " + vertical + " | " + waypointName(p, locale);
}
void atomicText(std::filesystem::path const& path, std::string const& text) {
    std::filesystem::create_directories(path.parent_path());
    auto temp  = path;
    temp      += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Save Wayfinder data");
}
Navigation readNavigation(std::filesystem::path const& path, std::string const& identity) {
    Navigation nav;
    if (!std::filesystem::exists(path)) return nav;
    if (std::filesystem::file_size(path) > 1024 * 1024) throw std::runtime_error("Waypoint file too large");
    std::ifstream in(path);
    auto          j = nlohmann::json::parse(in);
    if (j.at("version") != 1 || j.at("identity") != identity)
        throw std::runtime_error("Waypoint world/version mismatch");
    auto const& entries = j.at("points");
    if (!entries.is_array() || entries.size() > 517) throw std::runtime_error("Invalid waypoint list");
    std::unordered_set<std::uint64_t> ids;
    for (auto const& entry : entries) {
        Waypoint p;
        p.id        = entry.at("id").get<std::uint64_t>();
        p.name      = entry.at("name").get<std::string>();
        p.group     = entry.value("group", std::string{});
        p.dimension = entry.at("dimension");
        p.x         = entry.at("x");
        p.z         = entry.at("z");
        if (!entry.at("y").is_null()) p.y = entry.at("y").get<int>();
        p.color   = entry.at("color");
        p.icon    = entry.at("icon");
        p.death   = entry.at("death");
        p.created = entry.at("created");
        p.favorite = entry.value("favorite", false);
        validateWaypoint(p);
        if (!p.id || p.id == std::numeric_limits<std::uint64_t>::max() || !ids.insert(p.id).second)
            throw std::runtime_error("Invalid/duplicate waypoint ID");
        nav.nextId = std::max(nav.nextId, p.id + 1);
        nav.points.push_back(std::move(p));
    }
    if (std::count_if(nav.points.begin(), nav.points.end(), [](auto const& p) { return p.death; }) > 5
        || std::count_if(nav.points.begin(), nav.points.end(), [](auto const& p) { return !p.death; }) > 512)
        throw std::runtime_error("Waypoint/death history limit exceeded");
    nav.target = j.value("target", std::uint64_t{});
    if (!nav.find(nav.target)) nav.target = 0;
    auto readGroups = [&](char const* key, bool fullscreen) {
        auto found = j.find(key);
        if (found == j.end()) return;
        if (!found->is_array() || found->size() > 1024) throw std::runtime_error("Invalid waypoint groups");
        for (auto const& value : *found) {
            auto group = value.get<std::string>();
            if (cleanGroup(group) != group) throw std::runtime_error("Invalid waypoint group");
            if (nav.groupVisible(group, fullscreen)) nav.toggleGroup(group, fullscreen);
        }
    };
    readGroups("hiddenMinimapGroups", false);
    readGroups("hiddenFullMapGroups", true);
    return nav;
}
void writeNavigation(std::filesystem::path const& path, std::string const& identity, Navigation const& nav) {
    for (auto const* groups : {&nav.hiddenMinimapGroups, &nav.hiddenFullMapGroups}) {
        if (groups->size() > 1024) throw std::runtime_error("Too many hidden waypoint groups");
        for (auto const& group : *groups)
            if (cleanGroup(group) != group) throw std::runtime_error("Invalid waypoint group");
    }
    nlohmann::json j{
        {"version",  1                      },
        {"identity", identity               },
        {"target",   nav.target             },
        {"hiddenMinimapGroups", nav.hiddenMinimapGroups},
        {"hiddenFullMapGroups", nav.hiddenFullMapGroups},
        {"points",   nlohmann::json::array()}
    };
    for (auto const& p : nav.points) {
        validateWaypoint(p);
        j["points"].push_back({
            {"id",        p.id                                                },
            {"name",      p.name                                              },
            {"group",     p.group                                             },
            {"dimension", p.dimension                                         },
            {"x",         p.x                                                 },
            {"z",         p.z                                                 },
            {"y",         p.y ? nlohmann::json(*p.y) : nlohmann::json(nullptr)},
            {"color",     p.color                                             },
            {"icon",      p.icon                                              },
            {"death",     p.death                                             },
            {"created",   p.created                                           },
            {"favorite",  p.favorite                                          }
        });
    }
    atomicText(path, j.dump(2));
}
} // namespace wayfinder
