#include "wayfinder/Settings.h"
#include "wayfinder/EntityRadar.h"
#include "wayfinder/Navigation.h"
#include <fstream>
#include <nlohmann/json.hpp>

namespace wayfinder {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    Settings,
    fullMapKey,
    toggleMinimapKey,
    minimapZoomInKey,
    minimapZoomOutKey,
    sampleRadiusChunks,
    columnsPerTick,
    samplingBudgetMicros,
    maxCachedChunks,
    minimapPixels,
    fullscreenPixels,
    terrainPixelScale,
    refreshMilliseconds,
    autosaveSeconds,
    minimapSize,
    minimapBlocksPerPixel,
    showMinimap,
    includeWater,
    includeLeaves,
    terrainMode,
    caveSwitchY,
    cacheProfile,
    minimapOpacity,
    minimapPositionX,
    minimapPositionY,
    showCoordinates,
    showWaypoints,
    showNavigation,
    recordDeaths,
    recordTrail,
    showTrail,
    showScale,
    showCompass,
    showChunkBorders,
    showSlimeChunks,
    showBiomeRegions,
    showLighting,
    showBiome,
    showEntities,
    entityPortraits,
    minimapEntityScale,
    undergroundEntityOpacity,
    entityDepthThreshold,
    entityFilterEnabled,
    entityRadius,
    entityTypes,
    language
)
bool bindableMapKey(int code) {
    return (code >= 'A' && code <= 'Z' && code != 'W' && code != 'A' && code != 'S' && code != 'D' && code != 'F')
        || (code >= '0' && code <= '9') || (code >= 0x70 && code <= 0x7b);
}
bool bindableZoomKey(int code) { return bindableMapKey(code) || code == 187 || code == 189 || code == 107 || code == 109; }
bool zoomMinimap(Settings& s, bool zoomIn) {
    auto before = s.minimapBlocksPerPixel;
    s.minimapBlocksPerPixel = std::clamp(before * (zoomIn ? 0.8 : 1.25), minMapBlocksPerPixel, 16.0);
    return before != s.minimapBlocksPerPixel;
}
void applyPerformancePreset(Settings& s, PerformancePreset preset) {
    if (preset == PerformancePreset::Custom) return;
    bool light = preset == PerformancePreset::Light, quality = preset == PerformancePreset::Quality;
    s.sampleRadiusChunks = light ? 4 : quality ? 12 : 8;
    s.columnsPerTick = light ? 512 : quality ? 2048 : 1024;
    s.samplingBudgetMicros = light ? 750 : quality ? 2500 : 1500;
    s.minimapPixels = light ? 96 : quality ? 192 : 128;
    s.fullscreenPixels = light ? 256 : quality ? 512 : 384;
    s.terrainPixelScale = quality ? 4 : 2;
    s.refreshMilliseconds = light ? 200 : quality ? 75 : 100;
}
PerformancePreset performancePreset(Settings const& s) {
    for (auto p : {PerformancePreset::Light, PerformancePreset::Balanced, PerformancePreset::Quality}) {
        Settings candidate;
        applyPerformancePreset(candidate, p);
        if (s.sampleRadiusChunks == candidate.sampleRadiusChunks && s.columnsPerTick == candidate.columnsPerTick
            && s.samplingBudgetMicros == candidate.samplingBudgetMicros && s.minimapPixels == candidate.minimapPixels
            && s.fullscreenPixels == candidate.fullscreenPixels && s.terrainPixelScale == candidate.terrainPixelScale
            && s.refreshMilliseconds == candidate.refreshMilliseconds) return p;
    }
    return PerformancePreset::Custom;
}
void resetMapPreferences(Settings& s) {
    Settings defaults;
    defaults.cacheProfile = s.cacheProfile;
    defaults.maxCachedChunks = s.maxCachedChunks;
    defaults.autosaveSeconds = s.autosaveSeconds;
    s = std::move(defaults);
}
void normalizeSettings(Settings& s) {
    if (s.fullMapKey < 1 || s.fullMapKey > 255 || s.toggleMinimapKey < 1 || s.toggleMinimapKey > 255
        || s.fullMapKey == s.toggleMinimapKey || s.fullMapKey == 27 || s.toggleMinimapKey == 27)
        throw std::runtime_error("Map keys must be distinct virtual-key codes 1..255 (except Esc)");
    if (!bindableZoomKey(s.minimapZoomInKey) || !bindableZoomKey(s.minimapZoomOutKey)
        || s.minimapZoomInKey == s.minimapZoomOutKey || s.minimapZoomInKey == s.fullMapKey
        || s.minimapZoomInKey == s.toggleMinimapKey || s.minimapZoomOutKey == s.fullMapKey
        || s.minimapZoomOutKey == s.toggleMinimapKey)
        throw std::runtime_error("Map and zoom keys must be distinct");
    if (!std::isfinite(s.minimapSize) || !std::isfinite(s.minimapBlocksPerPixel) || !std::isfinite(s.minimapOpacity)
        || !std::isfinite(s.minimapPositionX) || !std::isfinite(s.minimapPositionY)
        || !std::isfinite(s.minimapEntityScale) || !std::isfinite(s.undergroundEntityOpacity))
        throw std::runtime_error("Map settings must be finite");
    s.sampleRadiusChunks    = std::clamp(s.sampleRadiusChunks, 1, 16);
    s.columnsPerTick        = std::clamp(s.columnsPerTick, 64, 8192);
    s.samplingBudgetMicros  = std::clamp(s.samplingBudgetMicros, 100, 5000);
    s.maxCachedChunks       = std::clamp(s.maxCachedChunks, 256, 65536);
    s.minimapPixels         = std::clamp(s.minimapPixels, 64, 256);
    s.fullscreenPixels      = std::clamp(s.fullscreenPixels, 128, 768);
    s.terrainPixelScale     = std::clamp(s.terrainPixelScale, 2, 4);
    s.refreshMilliseconds   = std::clamp(s.refreshMilliseconds, 50, 1000);
    s.autosaveSeconds       = std::clamp(s.autosaveSeconds, 10, 600);
    s.minimapSize           = std::clamp(s.minimapSize, 64.0f, 256.0f);
    s.minimapBlocksPerPixel = std::clamp(s.minimapBlocksPerPixel, minMapBlocksPerPixel, 16.0);
    s.minimapOpacity        = std::clamp(s.minimapOpacity, 0.2f, 1.0f);
    s.minimapPositionX      = std::clamp(s.minimapPositionX, 0.0f, 1.0f);
    s.minimapPositionY      = std::clamp(s.minimapPositionY, 0.0f, 1.0f);
    if (s.language != "auto" && s.language != "en_US" && s.language != "zh_CN") s.language = "auto";
    if (s.terrainMode != "auto" && s.terrainMode != "surface" && s.terrainMode != "cave") s.terrainMode = "auto";
    s.caveSwitchY = std::clamp(s.caveSwitchY, -64, 320);
    s.entityRadius = std::clamp(s.entityRadius, 16, 256);
    s.minimapEntityScale = std::clamp(s.minimapEntityScale, 0.25f, 2.0f);
    s.undergroundEntityOpacity = std::clamp(s.undergroundEntityOpacity, 0.1f, 1.0f);
    s.entityDepthThreshold = std::clamp(s.entityDepthThreshold, 1, 64);
    if (s.entityTypes.size() > 256) throw std::runtime_error("Too many entity types (maximum 256)");
    for (auto& type : s.entityTypes) type = entityTypeId(type);
    std::sort(s.entityTypes.begin(), s.entityTypes.end());
    s.entityTypes.erase(std::unique(s.entityTypes.begin(), s.entityTypes.end()), s.entityTypes.end());
    if (s.cacheProfile.size() > 1024) throw std::runtime_error("cacheProfile is too long");
}
Settings loadSettings(std::filesystem::path const& directory) {
    auto     path = directory / "config.json";
    Settings result;
    if (std::filesystem::exists(path)) {
        std::ifstream in(path);
        auto json = nlohmann::json::parse(in);
        result = json.get<Settings>();
        if (!json.contains("terrainPixelScale")) {
            // Upgrade existing Quality configs; retain the old density for other presets/custom configs.
            auto candidate = result;
            candidate.terrainPixelScale = 4;
            if (performancePreset(candidate) == PerformancePreset::Quality) result.terrainPixelScale = 4;
        }
        // Pre-zoom configs may already use +/- for M/N. Pick free defaults only
        // for missing new fields; explicit conflicting bindings remain an error.
        auto defaultZoomKey = [&](int preferred, int other) {
            for (int candidate : {preferred, 187, 189, 107, 109, 122, 123})
                if (candidate != result.fullMapKey && candidate != result.toggleMinimapKey && candidate != other)
                    return candidate;
            return preferred;
        };
        if (!json.contains("minimapZoomInKey"))
            result.minimapZoomInKey = defaultZoomKey(result.minimapZoomInKey, result.minimapZoomOutKey);
        if (!json.contains("minimapZoomOutKey"))
            result.minimapZoomOutKey = defaultZoomKey(result.minimapZoomOutKey, result.minimapZoomInKey);
    }
    normalizeSettings(result);
    if (!std::filesystem::exists(path)) saveSettings(directory, result);
    return result;
}
void saveSettings(std::filesystem::path const& directory, Settings const& settings) {
    auto normalized = settings;
    normalizeSettings(normalized);
    atomicText(directory / "config.json", nlohmann::json(normalized).dump(4));
}
} // namespace wayfinder
