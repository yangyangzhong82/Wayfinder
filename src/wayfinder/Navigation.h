#pragma once
#include "wayfinder/Locale.h"
#include "wayfinder/MapCore.h"
#include <filesystem>
#include <stdexcept>
#include <string>

namespace wayfinder {
inline constexpr std::array<std::uint32_t, 8> markerColors{
    rgba(255, 213, 70),
    rgba(100, 210, 255),
    rgba(110, 230, 140),
    rgba(255, 110, 110),
    rgba(210, 140, 255),
    rgba(255, 170, 80),
    rgba(255, 150, 210),
    rgba(240, 240, 240)
};
inline constexpr std::array<char const*, 4> markerIcons{"Pin", "Home", "Portal", "Skull"};
struct Waypoint {
    std::uint64_t      id{};
    std::string        name{"Waypoint"};
    std::string        group; // Optional user label; old files default to ungrouped.
    int                dimension{}, x{}, z{};
    std::optional<int> y;
    int                color{}, icon{};
    bool               death{};
    std::int64_t       created{}; // Unix seconds, UTC.
    bool               operator==(Waypoint const&) const = default;
};
void        validateWaypoint(Waypoint const& point);
std::string cleanName(std::string const& text);
std::string cleanGroup(std::string const& text);
void        eraseLastCharacter(std::string& text);
std::string dimensionName(int dimension, Locale const& locale = {});
std::string waypointName(Waypoint const& point, Locale const& locale);
struct Navigation {
    std::vector<Waypoint> points;
    std::uint64_t         target{}, nextId{1};
    Waypoint const*       find(std::uint64_t id) const;
    std::uint64_t         save(Waypoint point);
    void                  remove(std::uint64_t id);
    void                  recordDeath(int dimension, double x, double y, double z, std::int64_t time);
};
struct WaypointQuery {
    enum class Sort { Recent, Distance, Name };
    std::string search;
    std::optional<int> dimension;
    std::optional<std::string> group; // nullopt = all; empty string = ungrouped.
    Sort sort{Sort::Recent};
    int playerDimension{};
    double playerX{}, playerZ{};
};
std::vector<Waypoint const*> queryWaypoints(Navigation const& navigation, WaypointQuery const& query,
                                           Locale const& locale = {});
// Arms only after an alive tick, avoiding false deaths when joining a death screen.
class DeathTracker {
public:
    bool observe(bool alive) {
        bool died = mArmed && !alive;
        mArmed    = alive;
        return died;
    }
    void reset() { mArmed = false; }

private:
    bool mArmed{};
};
struct MarkerProjection {
    double x{}, y{};
    bool   outside{};
    double dx{}, dy{};
};
MarkerProjection projectMarker(MapView const& view, double worldX, double worldZ, double width, double height);
std::string      targetDescription(Waypoint const& point, int dimension, double x, double z, Locale const& locale = {});
Navigation       readNavigation(std::filesystem::path const& path, std::string const& identity);
void writeNavigation(std::filesystem::path const& path, std::string const& identity, Navigation const& navigation);
void atomicText(std::filesystem::path const& path, std::string const& text);
} // namespace wayfinder
