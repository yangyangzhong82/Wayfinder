#pragma once
#include <string>

namespace wayfinder {
struct Settings {
    int    fullMapKey{77};       // M; Windows virtual-key codes, configurable in config.json.
    int    toggleMinimapKey{78}; // N
    int    sampleRadiusChunks{8};
    int    columnsPerTick{1024};
    int    samplingBudgetMicros{1500};
    int    maxCachedChunks{8192};
    int    minimapPixels{128};
    int    fullscreenPixels{384};
    int    refreshMilliseconds{100};
    int    autosaveSeconds{30};
    float  minimapSize{112.0f};
    double minimapBlocksPerPixel{2.0};
    bool   showMinimap{true};
    bool   includeWater{true};
    bool   includeLeaves{true};
    // Blank: local LevelId where available; multiplayer gets an isolated session.
    // Set a unique profile per remote world to opt into cross-session history.
    std::string cacheProfile;
};
} // namespace wayfinder
