#pragma once
#include <filesystem>
#include <iterator>
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
    int         terrainPixelScale{2}; // Raster density; Quality uses 4 without changing world coverage.
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
    int         minimapFrameColor{0};     // Index into minimapFrameColorNames; the last entry hides the frame.
    float       minimapInfoOpacity{0.8f}; // Background of the minimap header tab and info rows.
    float       minimapPositionX{1.0f}, minimapPositionY{0.0f};
    bool        showCoordinates{true}, showWaypoints{true}, showNavigation{true}, recordDeaths{true};
    bool        recordTrail{true}, showTrail{true};
    bool        showScale{true}, showCompass{true}, showChunkBorders{false}, showBiome{true};
    bool        showSlimeChunks{false};
    bool        showBiomeRegions{false};
    bool        showLighting{true};
    bool        showEntities{true}, entityFilterEnabled{false};
    bool        entityPortraits{true};
    bool        minimapEntityCircles{true}; // Independent minimap style; full map keeps entityPortraits.
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
// Translation keys for the minimap frame palette; MapRenderer holds the matching RGB values.
inline constexpr char const* minimapFrameColorNames[]{
    "frame.slate", "frame.teal", "frame.gold", "frame.silver", "frame.rose", "frame.violet", "frame.none"};
inline constexpr int minimapFrameColorCount = static_cast<int>(std::size(minimapFrameColorNames));
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
