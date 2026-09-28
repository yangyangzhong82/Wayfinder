#pragma once
#include "wayfinder/MapCore.h"
#include <string>

namespace wayfinder {
struct MapRect {
    float x{}, y{}, width{}, height{};
    bool  contains(float px, float py) const { return px >= x && py >= y && px < x + width && py < y + height; }
    bool intersects(MapRect const& other) const {
        return x < other.x + other.width && x + width > other.x && y < other.y + other.height && y + height > other.y;
    }
};
enum class UiAction {
    None,
    NewPlayer,
    List,
    Settings,
    Back,
    Previous,
    Next,
    OpenWaypoint,
    Name,
    X,
    Y,
    Z,
    Color,
    Icon,
    Save,
    Navigate,
    Delete,
    Keep,
    Stop,
    ShowMap,
    SizeDown,
    SizeUp,
    OpacityDown,
    OpacityUp,
    Left,
    Right,
    Up,
    Down,
    Coordinates,
    Waypoints,
    Navigation,
    Deaths,
    MapKey,
    ToggleKey,
    Locate,
    LocateJump,
    LocateWaypoint,
    ContextWaypoint,
    Teleport,
    Follow,
    Scale,
    Compass,
    ChunkBorders,
    Biome,
    Language,
    SettingsTab,
    TerrainMode,
    CaveHeightDown,
    CaveHeightUp,
    Search,
    DimensionFilter,
    GroupFilter,
    SortWaypoints,
    ClearFilters,
    Group,
    NavigateExisting,
    MinimapZoomIn,
    MinimapZoomOut,
    ZoomInKey,
    ZoomOutKey,
    Water,
    Leaves,
    Performance,
    ResetSettings,
    Entities, ShowEntities, EntityFilterMode, EntityPlayersOnly, EntityAll, EntityNone,
    EntityType, EntitySearch, EntityRadiusDown, EntityRadiusUp, EntityPortraits,
    EntityScaleDown, EntityScaleUp, EntityTransparencyDown, EntityTransparencyUp,
    EntityDepthDown, EntityDepthUp
};
struct UiButton {
    MapRect       rect;
    std::string   text;
    UiAction      action{};
    std::uint64_t id{};
    bool          selected{};
    std::string   value;
    bool          toggle{}, enabled{true};
    std::string   data; // Stable identifiers for list actions, independent of sort/filter order.
};
struct UiFrame {
    bool                  modal{};
    MapRect               panel;
    MapRect               backdrop;
    std::string           title, message;
    std::string           pagination;
    std::vector<UiButton> buttons;
    UiButton const*       hit(float x, float y) const {
        for (auto const& button : buttons)
            if (button.enabled && button.action != UiAction::None && button.rect.contains(x, y)) return &button;
        return nullptr;
    }
};
// Shared by the toolbar and map viewport so drawing and pointer bounds stay aligned.
inline float mapToolbarBottom(float width) { return width >= 360 ? 26.0f : 46.0f; }
inline MapRect fullMapArea(float width, float height) {
    float top = mapToolbarBottom(width) + 16;
    return {10, top, width - 20, std::max(24.0f, height - top - 48)};
}
} // namespace wayfinder
