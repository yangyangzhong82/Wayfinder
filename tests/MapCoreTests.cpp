#include "wayfinder/MapCore.h"
#include "wayfinder/MapStorage.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace wayfinder;
namespace {
int  checks{};
void check(bool result, char const* message) {
    ++checks;
    if (!result) throw std::runtime_error(message);
}
template <class Function>
void rejects(Function function, char const* message) {
    bool failed = false;
    try {
        function();
    } catch (std::exception const&) {
        failed = true;
    }
    check(failed, message);
}
} // namespace
int main() {
    std::filesystem::path directory = "build/test-data/wayfinder-core";
    try {
        check(floorDiv(-1, 16) == -1 && localBlock(-1) == 15, "negative block -1");
        check(floorDiv(-16, 16) == -1 && localBlock(-16) == 0, "negative chunk edge");
        check(floorDiv(-17, 16) == -2 && localBlock(-17) == 15, "negative second chunk");
        for (int i = -5000; i <= 5000; ++i) {
            check(
                localBlock(i) >= 0 && localBlock(i) < 16 && floorDiv(i, 16) * 16 + localBlock(i) == i,
                "coordinate round trip"
            );
        }
        MapCache cache(3);
        auto     green = MapCell{rgba(30, 160, 70), 64};
        auto     blue  = MapCell{rgba(20, 70, 200), -32};
        check(cache.put(0, -1, -17, green), "first sample");
        check(cache.get(0, -1, -17) == green, "negative lookup");
        check(!cache.get(1, -1, -17).known(), "dimension isolation");
        auto revision = cache.revision();
        check(
            !cache.put(0, -1, -17, green) && cache.revision() == revision,
            "unchanged sample should not rebuild texture"
        );
        check(!cache.put(0, -1, -17, {}) && cache.get(0, -1, -17) == green, "unknown must preserve history");
        cache.put(1, -1, -17, blue);
        auto bounds = cache.bounds(0);
        check(
            bounds && bounds->minX == -1 && bounds->maxX == 0 && bounds->minZ == -17 && bounds->maxZ == -16,
            "exploration bounds"
        );
        check(!cache.bounds(2), "empty dimension bounds");
        MapView view{0, 0, 2, 384, 256};
        auto    before = view.worldAt(45.5, 67.5);
        view.zoomAt(0.5, 45.5, 67.5);
        auto after = view.worldAt(45.5, 67.5);
        check(std::abs(before[0] - after[0]) < 1e-9 && std::abs(before[1] - after[1]) < 1e-9, "cursor-anchored zoom");
        view.zoomAt(1e9, 0, 0);
        check(view.blocksPerPixel == 128, "maximum zoom");
        view.zoomAt(1e-9, 0, 0);
        check(view.blocksPerPixel == 0.5, "minimum zoom");
        auto pixel = cache.rasterize(0, MapView{-0.5, -16.5, 1, 1, 1});
        check(pixel.size() == 1 && pixel[0] == green.color, "raster sample at negative position");
        auto fog = cache.rasterize(2, MapView{0, 0, 1, 2, 2});
        check(fog.size() == 4 && fog[0] != green.color && (fog[0] >> 24) == 255, "unknown terrain fog");

        std::filesystem::create_directories(directory);
        auto        file     = directory / "roundtrip.wfmap";
        std::string identity = "world:test/negative-coordinates";
        writeMap(file, identity, cache.snapshot());
        MapCache loaded(3);
        loaded.restore(readMap(file, identity, 3));
        check(loaded.get(0, -1, -17) == green && loaded.get(1, -1, -17) == blue, "storage round trip");
        check(readMap(file, identity, 1).size() == 1, "bounded file load");
        {
            MapCache relief(4);
            auto     base = rgba(200, 200, 200);
            relief.put(5, 0, -1, {base, 10});
            relief.put(5, 0, 0, {base, 11});
            relief.put(5, 1, -1, {base, 10});
            relief.put(5, 1, 0, {base, 9});
            auto shaded = relief.rasterize(5, MapView{1, 0.5, 1, 2, 1});
            check((shaded[0] & 255u) > 200 && (shaded[1] & 255u) < 180, "north-facing relief shading");
            MapCache sea(4);
            auto     water = MapCell{rgba(60, 100, 220), 62, 1};
            sea.put(6, 0, 0, water);
            sea.put(6, 2, 0, {water.color, 62, 30});
            auto depth = sea.rasterize(6, MapView{0.5, 0.5, 1, 1, 1});
            auto deep  = sea.rasterize(6, MapView{2.5, 0.5, 1, 1, 1});
            check(((depth[0] >> 16) & 255u) > ((deep[0] >> 16) & 255u), "deep water renders darker");
            auto seaFile = directory / "depth.wfmap";
            writeMap(seaFile, identity, sea.snapshot());
            MapCache reread(4);
            reread.restore(readMap(seaFile, identity, 4));
            check(reread.get(6, 2, 0).depth == 30 && reread.get(6, 2, 0).height == 62, "depth round trip");
        }
        rejects([&] { readMap(file, "different-world", 3); }, "reject another world");
        loaded.put(0, -1, -17, blue);
        writeMap(file, identity, loaded.snapshot());
        MapCache replaced(3);
        replaced.restore(readMap(file, identity, 3));
        check(replaced.get(0, -1, -17) == blue, "atomic replace existing map");
        auto damaged = directory / "truncated.wfmap";
        std::filesystem::copy_file(file, damaged, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::resize_file(damaged, std::filesystem::file_size(damaged) - 1);
        rejects([&] { readMap(damaged, identity, 3); }, "reject truncated map");
        check(
            storageName(identity) == storageName(identity) && storageName("../escape").find("/") == std::string::npos,
            "safe stable filename"
        );

        MapCache small(2);
        small.put(0, 0, 0, green);
        small.put(0, 16, 0, green);
        small.put(0, 0, 0, green);
        small.put(0, 32, 0, green);
        check(
            small.size() == 2 && small.get(0, 0, 0).known() && !small.get(0, 16, 0).known(),
            "least recently sampled tile eviction"
        );
        std::cout << "PASS: " << checks << " map core/storage checks" << std::endl;
        return 0;
    } catch (std::exception const& ex) {
        std::cerr << "FAIL: " << ex.what() << std::endl;
        return 1;
    }
}
