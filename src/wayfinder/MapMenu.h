#pragma once
#include "wayfinder/Locale.h"
#include "wayfinder/MapUi.h"
#include "wayfinder/Navigation.h"
#include "wayfinder/Settings.h"
#include <set>

namespace wayfinder {
class MapMenu {
public:
    enum class Page { Map, List, Edit, Settings, Locate, Context, Entities, Explore, Groups };
    Locale      locale;
    Page        page{Page::Map};
    UiAction    focus{UiAction::None};
    std::string message, text;
    std::string entitySearch;
    std::vector<std::string> observedEntityTypes;
    int loadedEntities{}, nearbyEntities{}, visibleEntities{};
    std::vector<MapLayer> knownLayers;
    MapLayer displayedLayer;
    bool layerLocked{}, retracing{}, confirmClearTrail{}, contextCanTeleport{true}, trailSaveFailed{};
    std::size_t trailPoints{};
    Waypoint    draft;
    WaypointQuery query;
    bool batchMode{};
    std::set<std::uint64_t> selectedWaypoints;
    std::string batchGroup;
    int         offset{}, visibleRows{1}, rowCount{};
    int         settingsTab{};
    float       contextX{}, contextY{};
    bool        contextLoading{};
    bool        confirmDelete{}, confirmReset{}, selectAll{}, errorMessage{};
    void        fail(std::string error) {
        message      = std::move(error);
        errorMessage = true;
    }
    void open(Page next);
    void edit(Waypoint point);
    void context(Waypoint point, float x, float y);
    void scroll(int direction);
    void beginField(UiAction field);
    void commitField();
    void cancelField();
    void append(std::string const& input);
    void backspace();
    bool textField() const;
    // Settings/navigation are caller-owned copies, committed only after saving.
    // Returns 1 for navigation changes, 2 for settings, 3 to locate, 4 to create at located coordinates.
    int     action(UiButton const& button, Settings& settings, Navigation& navigation);
    UiFrame build(float width, float height, Settings const& settings, Navigation const& navigation);
};
} // namespace wayfinder
