#pragma once
#include <cstdint>
#include <functional>
#include <limits>

namespace wayfinder {
// Surface maps retain their original dimension and filenames. Underground maps
// have an explicit, signed eight-block slice; never encode a layer as a dimension.
inline constexpr int surfaceSlice    = std::numeric_limits<int>::max();
inline constexpr int caveSliceHeight = 8;
struct MapLayer {
    int dimension{};
    int slice{surfaceSlice};
    MapLayer() = default;
    MapLayer(int dimension, int slice = surfaceSlice) : dimension(dimension), slice(slice) {}
    bool underground() const { return slice != surfaceSlice; }
    int  referenceY() const { return slice * caveSliceHeight + caveSliceHeight / 2; }
    bool operator==(MapLayer const&) const = default;
};
struct MapLayerHash {
    std::size_t operator()(MapLayer layer) const noexcept {
        return std::hash<std::uint64_t>{}(
            (std::uint64_t(static_cast<std::uint32_t>(layer.dimension)) << 32) | static_cast<std::uint32_t>(layer.slice)
        );
    }
};
} // namespace wayfinder
