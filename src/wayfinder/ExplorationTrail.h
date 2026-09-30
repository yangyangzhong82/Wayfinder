#pragma once
#include "wayfinder/MapUi.h"
#include "wayfinder/Navigation.h"
#include <span>

namespace wayfinder {
struct TrailPoint {
    MapLayer layer;
    int x{}, y{}, z{};
    std::int64_t time{};
    bool start{true};
    bool operator==(TrailPoint const&) const = default;
};
class ExplorationTrail {
public:
    static constexpr std::size_t limit = 8192;
    std::vector<TrailPoint> points;
    std::uint64_t revision{};
    bool observe(MapLayer layer, double x, double y, double z, std::int64_t time, bool recording);
    void breakSegment();
    void clear();
    bool startRetrace(MapLayer layer, double x, double y, double z);
    void stopRetrace();
    std::optional<std::size_t> retraceIndex() const { return mRetrace; }
    std::optional<Waypoint> retraceTarget(Locale const& locale = {}) const;
private:
    std::optional<TrailPoint> mPrevious;
    std::optional<std::size_t> mRetrace;
    bool mBreak{true};
};
struct TrailLine { float x1{}, y1{}, x2{}, y2{}; };
std::optional<TrailLine> projectTrailLine(TrailPoint const& a, TrailPoint const& b, MapLayer layer,
                                         MapView const& view, MapRect const& area);
void overlayTrail(std::span<std::uint32_t> pixels, MapView const& view, MapLayer layer,
                  std::span<TrailPoint const> points, int pixelScale = 1);
ExplorationTrail readTrail(std::filesystem::path const& path, std::string const& identity);
void writeTrail(std::filesystem::path const& path, std::string const& identity, std::span<TrailPoint const> points);
} // namespace wayfinder
