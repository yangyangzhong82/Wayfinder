#pragma once
#include "wayfinder/Navigation.h"
#include <iomanip>
#include <locale>
#include <sstream>

namespace wayfinder {
// Terrain heights denote the surface block, not the player's feet.
inline std::string teleportCommand(Waypoint const& point, int dimension) {
    validateWaypoint(point);
    if (point.dimension != dimension) throw std::runtime_error("Teleport destination is in another dimension.");
    if (!point.y) throw std::runtime_error("Unknown height; teleport unavailable here.");
    if (*point.y >= 32767) throw std::runtime_error("Coordinates outside world bounds");
    std::ostringstream command;
    command.imbue(std::locale::classic());
    // Do not require a collision check against an unloaded server destination.
    // Client chunk availability cannot tell us whether that server area is loaded.
    command << std::fixed << std::setprecision(1) << "/tp @s " << (point.x + 0.5) << ' '
            << (*point.y + 1) << ' ' << (point.z + 0.5) << " false";
    return command.str();
}
} // namespace wayfinder
