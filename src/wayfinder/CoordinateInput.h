#pragma once
#include "wayfinder/Navigation.h"
#include <charconv>
#include <string_view>

namespace wayfinder {
// A pasted pair is X/Z; a triple is X/Y/Z. Validate a copy before changing the draft.
inline Waypoint withCoordinates(Waypoint point, std::string_view input) {
    std::array<int, 3> values{};
    std::size_t count{};
    while (!input.empty()) {
        auto first = input.find_first_not_of(" \t\r\n,");
        if (first == std::string_view::npos) break;
        input.remove_prefix(first);
        auto end = input.find_first_of(" \t\r\n,");
        auto token = input.substr(0, end);
        if (token.starts_with('+')) {
            token.remove_prefix(1);
            if (token.empty() || token.front() < '0' || token.front() > '9')
                throw std::runtime_error("Enter X Z or X Y Z integers");
        }
        if (token.empty() || count == values.size()) throw std::runtime_error("Enter X Z or X Y Z integers");
        auto [last, error] = std::from_chars(token.data(), token.data() + token.size(), values[count]);
        if (error != std::errc{} || last != token.data() + token.size())
            throw std::runtime_error("Enter X Z or X Y Z integers");
        ++count;
        if (end == std::string_view::npos) break;
        input.remove_prefix(end);
    }
    if (count != 2 && count != 3) throw std::runtime_error("Enter X Z or X Y Z integers");
    point.x = values[0];
    point.z = values[count - 1];
    if (count == 3) point.y = values[1];
    validateWaypoint(point);
    return point;
}
} // namespace wayfinder
