#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace wayfinder {
struct Settings {
    int         fullMapKey{77};       // M; Windows virtual-key codes, configurable in config.json.
    int         toggleMinimapKey{78}; // N
    int         minimapZoomInKey{187}, minimapZoomOutKey{189}; // + / -
    int         sampleRadiusChunks{8};
    int         columnsPerTick{1024};
    int         samplingBudgetMicros{1500};
    int         maxCachedChunks{8192};
    int         minimapPixels{128};
    int         fullscreenPixels{384};
    int         refreshMilliseconds{100};
    int         autosaveSeconds{30};
    float       minimapSize{112.0f};
    double      minimapBlocksPerPixel{2.0};
    bool        showMinimap{true};
    bool        includeWater{true};
    bool        includeLeaves{true};
    std::string terrainMode{"auto"}; // auto / surface / cave; cave height follows the player.
    int         caveSwitchY{48}; // Overworld auto mode: enter at/below this Y, exit four blocks higher.
    float       minimapOpacity{0.92f};
    float       minimapPositionX{1.0f}, minimapPositionY{0.0f};
    bool        showCoordinates{true}, showWaypoints{true}, showNavigation{true}, recordDeaths{true};
    bool        showScale{true}, showCompass{true}, showChunkBorders{false}, showBiome{true};
    bool        showEntities{true}, entityFilterEnabled{false};
    bool        entityPortraits{true};
    float       minimapEntityScale{0.75f};
    float       undergroundEntityOpacity{0.35f}; // Lower entities: 65% transparent by default.
    int         entityDepthThreshold{4}; // Fade entities at least this many blocks below the player.
    int         entityRadius{64};
    std::vector<std::string> entityTypes; // Exact identifiers; an enabled empty whitelist shows nothing.
    std::string language{"auto"}; // auto follows Minecraft; en_US and zh_CN override.
    // Blank: use LevelId where available, regardless of the multiplayer setting.
    // Without a LevelId, use an isolated session.
    // Missing storage identity must never block the live map or its hotkeys.
    // Set a unique profile per remote world to opt into cross-session history.
    std::string cacheProfile;
};
Settings loadSettings(std::filesystem::path const& directory);
void     saveSettings(std::filesystem::path const& directory, Settings const& settings);
void     normalizeSettings(Settings& settings);
bool     bindableMapKey(int code);
bool     bindableZoomKey(int code);
bool     zoomMinimap(Settings& settings, bool zoomIn);
enum class PerformancePreset { Light, Balanced, Quality, Custom };
PerformancePreset performancePreset(Settings const& settings);
void applyPerformancePreset(Settings& settings, PerformancePreset preset);
void resetMapPreferences(Settings& settings);
} // namespace wayfinder
