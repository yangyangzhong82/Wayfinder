#pragma once

#include <string>
#include <string_view>

namespace wayfinder {
// Persistence metadata must never gate the live map or its keyboard handlers.
// Keep the historical "local:" prefix so existing LevelId files remain readable.
// Use an isolated session when a world identity is unavailable.
inline std::string
worldStorageIdentity(std::string_view profile, std::string_view levelId, std::string_view sessionToken) {
    if (!profile.empty()) return "profile:" + std::string(profile);
    // Multiplayer can be enabled on a local world. It must not veto its LevelId.
    if (!levelId.empty()) return "local:" + std::string(levelId);
    return "session:" + std::string(sessionToken);
}
} // namespace wayfinder
