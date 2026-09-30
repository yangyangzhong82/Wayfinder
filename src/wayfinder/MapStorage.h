#pragma once

#include "wayfinder/MapCore.h"
#include <filesystem>
#include <string>
#include <string_view>

namespace wayfinder {
std::string             storageName(std::string_view identity);
std::vector<TileRecord> readMap(std::filesystem::path const& path, std::string const& identity, std::size_t capacity,
                                TerrainMaterialPool* materials = nullptr);
void writeMap(std::filesystem::path const& path, std::string const& identity, std::vector<TileRecord> const& tiles);
} // namespace wayfinder
