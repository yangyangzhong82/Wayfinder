#include "wayfinder/ExplorationTrail.h"
#include <fstream>
#include <nlohmann/json.hpp>

namespace wayfinder {
namespace {
bool valid(TrailPoint const& p) {
    return std::abs(static_cast<double>(p.x)) <= 29999984 && std::abs(static_cast<double>(p.z)) <= 29999984
        && p.y >= -32768 && p.y <= 32767 && p.time >= 0
        && (!p.layer.underground() || p.layer.slice == floorDiv(p.y, caveSliceHeight));
}
bool discontinuous(TrailPoint const& a, TrailPoint const& b) {
    return a.layer.dimension != b.layer.dimension || b.time < a.time || b.time - a.time > 5
        || std::hypot(double(a.x) - b.x, double(a.y) - b.y, double(a.z) - b.z) > 32;
}
} // namespace
void ExplorationTrail::breakSegment() { mPrevious.reset(); mBreak = true; }
void ExplorationTrail::stopRetrace() { mRetrace.reset(); breakSegment(); }
void ExplorationTrail::clear() { points.clear(); ++revision; stopRetrace(); }
bool ExplorationTrail::observe(MapLayer layer, double x, double y, double z, std::int64_t time, bool recording) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || std::abs(x) > 29999984
        || std::abs(z) > 29999984 || y < -32768 || y > 32767 || time < 0) { stopRetrace(); return false; }
    TrailPoint point{layer, blockCoordinate(x), blockCoordinate(y), blockCoordinate(z), time, true};
    if (!valid(point)) { stopRetrace(); return false; }
    bool jump = mPrevious && discontinuous(*mPrevious, point);
    if (jump) mBreak = true;
    mPrevious = point;
    if (mRetrace) {
        auto const& target = points[*mRetrace];
        if (jump || target.layer.dimension != layer.dimension || std::hypot(target.x + 0.5 - x, target.y - y, target.z + 0.5 - z) > 32) {
            stopRetrace(); return false;
        }
        while (mRetrace) {
            auto const& next = points[*mRetrace];
            if (std::hypot(next.x + 0.5 - x, next.z + 0.5 - z) >= 2 || std::abs(next.y - y) >= 2) break;
            if (next.start || *mRetrace == 0) stopRetrace();
            else --*mRetrace;
        }
        return false; // Retracing never records a second copy of the return trip.
    }
    if (!recording) { breakSegment(); return false; }
    if (!mBreak && !points.empty() && points.back().layer == layer && std::hypot(double(point.x) - points.back().x,
        double(point.y) - points.back().y, double(point.z) - points.back().z) < 2) return false;
    point.start = mBreak || points.empty();
    if (points.size() == limit) {
        points.erase(points.begin(), points.begin() + 256);
        points.front().start = true;
    }
    points.push_back(point);
    ++revision;
    mBreak = false;
    return true;
}
bool ExplorationTrail::startRetrace(MapLayer layer, double x, double y, double z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    std::optional<std::size_t> nearest;
    double best = 16;
    for (std::size_t i = 1; i < points.size(); ++i) {
        auto const& p = points[i];
        if (p.start || p.layer.dimension != layer.dimension || std::abs(p.y - y) >= 4) continue;
        double distance = std::hypot(p.x + 0.5 - x, p.z + 0.5 - z);
        if (distance <= best) { best = distance; nearest = i; }
    }
    if (!nearest) return false;
    mRetrace = nearest;
    breakSegment();
    return true;
}
std::optional<Waypoint> ExplorationTrail::retraceTarget(Locale const& locale) const {
    if (!mRetrace) return {};
    auto const& p = points[*mRetrace];
    Waypoint target;
    target.name = locale.tr("Retracing");
    target.dimension = p.layer.dimension;
    target.x = p.x; target.y = p.y; target.z = p.z;
    return target;
}
std::optional<TrailLine> projectTrailLine(TrailPoint const& a, TrailPoint const& b, MapLayer layer,
                                         MapView const& view, MapRect const& area) {
    if (b.start || a.layer.dimension != layer.dimension || b.layer.dimension != layer.dimension || area.width <= 0 || area.height <= 0
        || view.width <= 0 || view.height <= 0 || view.blocksPerPixel <= 0) return {};
    double start = 0, finish = 1;
    if (a.layer == b.layer && a.layer != layer) return {};
    // Clip a continuous stair/ramp segment to this height slice. Never draw a
    // connecting line between different dimensions or across an explicit break.
    if (layer.underground()) {
        double low = double(layer.slice) * caveSliceHeight, high = low + caveSliceHeight;
        double dy = double(b.y) - a.y;
        if (dy == 0) {
            if (a.layer != layer || b.layer != layer) return {};
        } else {
            start = std::max(0.0, std::min((low - a.y) / dy, (high - a.y) / dy));
            finish = std::min(1.0, std::max((low - a.y) / dy, (high - a.y) / dy));
            if (finish <= start) return {};
        }
    } else {
        if (a.layer != layer && b.layer != layer) return {};
        if (a.layer != layer) start = 0.5;
        if (b.layer != layer) finish = 0.5;
    }
    auto origin = view.worldAt(0, 0);
    double x = (a.x + 0.5 - origin[0]) / (view.width * view.blocksPerPixel) * area.width;
    double y = (a.z + 0.5 - origin[1]) / (view.height * view.blocksPerPixel) * area.height;
    double dx = (double(b.x) - a.x) / (view.width * view.blocksPerPixel) * area.width;
    double dy = (double(b.z) - a.z) / (view.height * view.blocksPerPixel) * area.height;
    double first = start, last = finish;
    auto clip = [&](double p, double q) {
        if (p == 0) return q >= 0;
        double t = q / p;
        if (p < 0) first = std::max(first, t); else last = std::min(last, t);
        return first <= last;
    };
    if (!clip(-dx, x) || !clip(dx, area.width - x) || !clip(-dy, y) || !clip(dy, area.height - y)) return {};
    return TrailLine{area.x + float(x + first * dx), area.y + float(y + first * dy),
                     area.x + float(x + last * dx), area.y + float(y + last * dy)};
}
void overlayTrail(std::span<std::uint32_t> pixels, MapView const& view, MapLayer layer,
                  std::span<TrailPoint const> points, int pixelScale) {
    if (view.width <= 0 || view.height <= 0 || pixels.size() != static_cast<std::size_t>(view.width) * view.height) return;
    // Trace in logical pixels so denser terrain does not make the trail thinner.
    if (pixelScale < 1 || pixelScale > 4 || view.width % pixelScale || view.height % pixelScale) return;
    auto logical = view;
    logical.width /= pixelScale;
    logical.height /= pixelScale;
    logical.blocksPerPixel *= pixelScale;
    MapRect area{0, 0, float(logical.width), float(logical.height)};
    int budget = 65536;
    // Paint only when uploading the terrain texture, not as thousands of GUI draw calls per frame.
    // Latest visible segments take priority; clip in pixel space before bounded stepping.
    for (std::size_t i = points.size(); i > 1 && budget > 0; --i) {
        auto line = projectTrailLine(points[i - 2], points[i - 1], layer, logical, area);
        if (!line) continue;
        int x = std::clamp(static_cast<int>(std::floor(line->x1)), 0, logical.width - 1);
        int y = std::clamp(static_cast<int>(std::floor(line->y1)), 0, logical.height - 1);
        int endX = std::clamp(static_cast<int>(std::floor(line->x2)), 0, logical.width - 1);
        int endY = std::clamp(static_cast<int>(std::floor(line->y2)), 0, logical.height - 1);
        int dx = std::abs(endX - x), dy = -std::abs(endY - y);
        int sx = x < endX ? 1 : -1, sy = y < endY ? 1 : -1, error = dx + dy;
        // Integer stepping prevents floating-point rounding from skipping individual pixels.
        while (budget > 0) {
            for (int dz = 0; dz < pixelScale; ++dz) for (int dx = 0; dx < pixelScale; ++dx) {
                auto& pixel = pixels[static_cast<std::size_t>(y * pixelScale + dz) * view.width + x * pixelScale + dx];
                pixel = rgba(((pixel & 255) + 170) / 3, (((pixel >> 8) & 255) + 420) / 3,
                             (((pixel >> 16) & 255) + 460) / 3);
            }
            --budget;
            if (x == endX && y == endY) break;
            int twice = 2 * error;
            if (twice >= dy) { error += dy; x += sx; }
            if (twice <= dx) { error += dx; y += sy; }
        }
    }
}
ExplorationTrail readTrail(std::filesystem::path const& path, std::string const& identity) {
    ExplorationTrail trail;
    if (!std::filesystem::exists(path)) return trail;
    if (std::filesystem::file_size(path) > 2 * 1024 * 1024) throw std::runtime_error("Trail file too large");
    auto j = nlohmann::json::parse(std::ifstream(path));
    int version = j.at("version").get<int>();
    if ((version != 1 && version != 2) || j.at("identity") != identity) throw std::runtime_error("Trail world/version mismatch");
    auto const& entries = j.at("points");
    if (!entries.is_array() || entries.size() > ExplorationTrail::limit) throw std::runtime_error("Invalid trail list");
    for (auto const& entry : entries) {
        TrailPoint p{{entry.at("dimension").get<int>(), entry.at("slice").get<int>()}, entry.at("x").get<int>(),
            entry.at("y").get<int>(), entry.at("z").get<int>(), entry.at("time").get<std::int64_t>(), entry.at("start").get<bool>()};
        if (!valid(p)) throw std::runtime_error("Invalid trail point");
        if (trail.points.empty()) p.start = true;
        else if (trail.points.back().layer.dimension != p.layer.dimension
            || (version == 1 && trail.points.back().layer != p.layer)) p.start = true;
        trail.points.push_back(p);
    }
    return trail; // A fresh session always starts a separate segment.
}
void writeTrail(std::filesystem::path const& path, std::string const& identity, std::span<TrailPoint const> points) {
    if (points.size() > ExplorationTrail::limit) throw std::runtime_error("Trail point limit exceeded");
    nlohmann::json j{{"version", 2}, {"identity", identity}, {"points", nlohmann::json::array()}};
    for (auto const& p : points) {
        if (!valid(p)) throw std::runtime_error("Invalid trail point");
        j["points"].push_back({{"dimension", p.layer.dimension}, {"slice", p.layer.slice}, {"x", p.x},
            {"y", p.y}, {"z", p.z}, {"time", p.time}, {"start", p.start}});
    }
    atomicText(path, j.dump());
}
} // namespace wayfinder
