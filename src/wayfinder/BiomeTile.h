#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wayfinder {
// Local palette: no global IDs to collide or leak between worlds. Zero is unknown.
struct BiomeTile {
    std::array<std::uint16_t, 256> cells{};
    std::vector<std::string> names;
    std::string_view get(std::size_t index) const {
        auto id = cells[index];
        return id && id <= names.size() ? std::string_view(names[id - 1]) : std::string_view{};
    }
    bool set(std::size_t index, std::string_view name) {
        if (name.empty() || name.size() > 256 || get(index) == name) return false;
        auto found = std::find(names.begin(), names.end(), name);
        if (found != names.end()) cells[index] = static_cast<std::uint16_t>(found - names.begin() + 1);
        else {
            // Reuse unused entries so edits/custom biomes cannot grow a palette forever.
            cells[index] = 0;
            std::array<bool, 257> used{};
            for (auto id : cells) if (id <= 256) used[id] = true;
            std::size_t slot = 0;
            while (slot < names.size() && used[slot + 1]) ++slot;
            if (slot == names.size()) names.emplace_back(name);
            else names[slot] = name;
            cells[index] = static_cast<std::uint16_t>(slot + 1);
        }
        return true;
    }
    // At distant scales, use the dominant biome of fully sampled chunks only.
    std::string overview() const {
        std::array<unsigned, 257> counts{};
        for (auto id : cells) {
            if (!id || id > names.size()) return {};
            ++counts[id];
        }
        auto best = std::max_element(counts.begin() + 1, counts.end());
        return names[static_cast<std::size_t>(best - counts.begin()) - 1];
    }
};
} // namespace wayfinder
