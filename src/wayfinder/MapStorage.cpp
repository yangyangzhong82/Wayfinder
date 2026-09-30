#include "wayfinder/MapStorage.h"

#include <bit>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace wayfinder {
namespace {
constexpr char magic[8]       = {'W', 'A', 'Y', 'M', 'A', 'P', '0', '7'};
constexpr char lightMagic[8]  = {'W', 'A', 'Y', 'M', 'A', 'P', '0', '6'};
constexpr char biomeMagic[8]  = {'W', 'A', 'Y', 'M', 'A', 'P', '0', '5'};
constexpr char layerMagic[8]  = {'W', 'A', 'Y', 'M', 'A', 'P', '0', '4'};
constexpr char touchedMagic[8] = {'W', 'A', 'Y', 'M', 'A', 'P', '0', '3'};
constexpr char depthMagic[8]  = {'W', 'A', 'Y', 'M', 'A', 'P', '0', '2'};
constexpr char legacyMagic[8] = {'W', 'A', 'Y', 'M', 'A', 'P', '0', '1'}; // No water depth; read-only.
void           put32(std::ostream& stream, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) stream.put(static_cast<char>((value >> shift) & 255u));
}
std::uint32_t get32(std::istream& stream) {
    std::uint32_t value{};
    for (unsigned shift = 0; shift < 32; shift += 8) {
        auto ch = stream.get();
        if (ch == std::char_traits<char>::eof()) throw std::runtime_error("Truncated Wayfinder map file");
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(ch)) << shift;
    }
    return value;
}
} // namespace

std::string storageName(std::string_view identity) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : identity) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    std::ostringstream name;
    name << std::hex << std::setw(16) << std::setfill('0') << hash;
    return name.str() + ".wfmap";
}

std::vector<TileRecord> readMap(std::filesystem::path const& path, std::string const& identity, std::size_t capacity,
                                TerrainMaterialPool* materials) {
    if (!std::filesystem::exists(path)) return {};
    std::ifstream       stream(path, std::ios::binary);
    std::array<char, 8> header{};
    stream.read(header.data(), header.size());
    bool legacy      = stream && std::equal(header.begin(), header.end(), std::begin(legacyMagic));
    bool current     = stream && std::equal(header.begin(), header.end(), std::begin(magic));
    bool layerFormat = stream && std::equal(header.begin(), header.end(), std::begin(layerMagic));
    bool lightFormat = current || (stream && std::equal(header.begin(), header.end(), std::begin(lightMagic)));
    bool biomeFormat = lightFormat || (stream && std::equal(header.begin(), header.end(), std::begin(biomeMagic)));
    bool layered = biomeFormat || layerFormat;
    bool depthFormat = stream && std::equal(header.begin(), header.end(), std::begin(depthMagic));
    bool touchedFormat = stream && std::equal(header.begin(), header.end(), std::begin(touchedMagic));
    if (!stream || (!legacy && !layered && !depthFormat && !touchedFormat)) throw std::runtime_error("Unsupported Wayfinder map format");
    auto length = get32(stream);
    if (length > 4096) throw std::runtime_error("Invalid map identity length");
    std::string savedIdentity(length, char{});
    stream.read(savedIdentity.data(), length);
    if (!stream || savedIdentity != identity) throw std::runtime_error("Map belongs to a different world/profile");
    auto count = get32(stream);
    if (count > 65536) throw std::runtime_error("Map exceeds maximum supported tile count");
    auto expectedSize = 8ull + 4 + length + 4 + static_cast<std::uint64_t>(count) * ((layered ? 24 : touchedFormat ? 20 : 12) + 256 * 8);
    if (!biomeFormat && std::filesystem::file_size(path) != expectedSize) throw std::runtime_error("Map record size mismatch");
    std::vector<TileRecord> records;
    TerrainMaterialPool localMaterials;
    if (!materials) materials = &localMaterials;
    records.reserve(std::min<std::size_t>(count, capacity));
    for (std::uint32_t n = 0; n < count; ++n) {
        TileRecord record{};
        record.key.dimension = std::bit_cast<std::int32_t>(get32(stream));
        record.key.x         = std::bit_cast<std::int32_t>(get32(stream));
        record.key.z         = std::bit_cast<std::int32_t>(get32(stream));
        if (layered) {
            record.key.slice = std::bit_cast<std::int32_t>(get32(stream));
            if (record.key.slice != surfaceSlice && (record.key.slice < -4096 || record.key.slice > 4095))
                throw std::runtime_error("Invalid map cave slice");
        }
        if (record.key.x < -1875000 || record.key.x > 1875000 || record.key.z < -1875000 || record.key.z > 1875000)
            throw std::runtime_error("Map chunk coordinates outside supported world bounds");
        if (layered || touchedFormat) {
            auto low = get32(stream), high = get32(stream);
            record.tile.lastTouched = low | (static_cast<std::uint64_t>(high) << 32);
        } else record.tile.lastTouched = n + 1;
        for (auto& cell : record.tile.cells) {
            cell.color = get32(stream);
            auto word  = get32(stream);
            if (legacy) {
                auto height = std::bit_cast<std::int32_t>(word);
                if (height < -32768 || height > 32767) throw std::runtime_error("Invalid map height");
                cell.height = static_cast<std::int16_t>(height);
            } else {
                // v2: low 16 bits signed height, bits 16-23 water depth, top byte reserved.
                auto flags = word >> 24;
                if ((!layered && flags) || flags > MapCell::voidSpace)
                    throw std::runtime_error("Invalid map cell reserved bits");
                cell.height = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(word & 0xffffu));
                cell.depth  = static_cast<std::uint8_t>(word >> 16);
                cell.flags  = static_cast<std::uint8_t>(flags);
            }
            if (cell.known() && (cell.color >> 24) != 255u) throw std::runtime_error("Invalid map pixel alpha");
        }
        if (lightFormat) {
            for (auto& cell : record.tile.cells) {
                auto light = get32(stream);
                auto sky = light & 255u, block = (light >> 8) & 255u;
                if ((light >> 16) || !((sky <= 15 && block <= 15) || (sky == 255 && block == 255)))
                    throw std::runtime_error("Invalid map light levels");
                cell.skyLight = static_cast<std::uint8_t>(sky);
                cell.blockLight = static_cast<std::uint8_t>(block);
            }
        }
        if (current) {
            auto count = get32(stream);
            if (count > 256) throw std::runtime_error("Invalid terrain material palette size");
            std::vector<TerrainMaterialPtr> palette;
            palette.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                TerrainMaterial::Pixels pixels;
                for (auto& pixel : pixels) pixel = get32(stream);
                auto material = std::make_shared<TerrainMaterial const>(pixels);
                if (!material->visible()) throw std::runtime_error("Empty terrain material");
                palette.push_back(materials->intern(std::move(material)));
            }
            for (auto& cell : record.tile.cells) {
                auto reference = get32(stream), tint = get32(stream);
                auto index = reference & 0xffffu, rotation = reference >> 16;
                if (index > count || rotation > 3 || (tint >> 24) != 255
                    || (index && (!cell.floor() || cell.depth)))
                    throw std::runtime_error("Invalid terrain material reference");
                if (index) cell.material = palette[index - 1];
                cell.tint = tint;
                cell.rotation = static_cast<std::uint8_t>(rotation);
                auto low = get32(stream), high = get32(stream);
                cell.surfaceId = low | (std::uint64_t(high) << 32);
            }
        }
        if (biomeFormat) {
            auto names = get32(stream);
            if (names > 256) throw std::runtime_error("Invalid biome palette size");
            for (std::uint32_t i = 0; i < names; ++i) {
                auto size = get32(stream);
                if (!size || size > 256) throw std::runtime_error("Invalid biome identifier length");
                std::string name(size, '\0');
                stream.read(name.data(), size);
                if (!stream || name.find('\0') != std::string::npos) throw std::runtime_error("Invalid biome identifier");
                record.tile.biomes.names.push_back(std::move(name));
            }
            for (auto& id : record.tile.biomes.cells) {
                auto value = get32(stream);
                if (value > names) throw std::runtime_error("Invalid biome palette index");
                id = static_cast<std::uint16_t>(value);
            }
        }
        if (records.size() < capacity) records.push_back(std::move(record));
    }
    if (stream.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Unexpected trailing map data");
    return records;
}

void writeMap(std::filesystem::path const& path, std::string const& identity, std::vector<TileRecord> const& tiles) {
    if (identity.size() > 4096 || tiles.size() > 65536) throw std::runtime_error("Map exceeds storage limits");
    std::filesystem::create_directories(path.parent_path());
    auto temporary  = path;
    temporary      += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        stream.write(magic, sizeof(magic));
        put32(stream, static_cast<std::uint32_t>(identity.size()));
        stream.write(identity.data(), static_cast<std::streamsize>(identity.size()));
        put32(stream, static_cast<std::uint32_t>(tiles.size()));
        for (auto const& record : tiles) {
            put32(stream, static_cast<std::uint32_t>(record.key.dimension));
            put32(stream, static_cast<std::uint32_t>(record.key.x));
            put32(stream, static_cast<std::uint32_t>(record.key.z));
            put32(stream, static_cast<std::uint32_t>(record.key.slice));
            put32(stream, static_cast<std::uint32_t>(record.tile.lastTouched));
            put32(stream, static_cast<std::uint32_t>(record.tile.lastTouched >> 32));
            for (auto const& cell : record.tile.cells) {
                put32(stream, cell.color);
                put32(
                    stream,
                    std::bit_cast<std::uint16_t>(cell.height) | (static_cast<std::uint32_t>(cell.depth) << 16)
                        | (static_cast<std::uint32_t>(cell.flags) << 24)
                );
            }
            for (auto const& cell : record.tile.cells) {
                if (!((cell.skyLight <= 15 && cell.blockLight <= 15) || (cell.skyLight == 255 && cell.blockLight == 255)))
                    throw std::runtime_error("Invalid map light levels");
                put32(stream, cell.skyLight | (static_cast<std::uint32_t>(cell.blockLight) << 8));
            }
            // One copy per distinct material in this tile. References survive
            // process restarts; no game pointers or runtime block IDs reach disk.
            std::vector<TerrainMaterialPtr> palette;
            std::array<std::uint32_t, 256> references{};
            for (std::size_t i = 0; i < record.tile.cells.size(); ++i) {
                auto const& cell = record.tile.cells[i];
                if (cell.rotation > 3 || (cell.tint >> 24) != 255
                    || (cell.material && (!cell.floor() || cell.depth || !cell.material->visible())))
                    throw std::runtime_error("Invalid terrain material reference");
                references[i] = std::uint32_t(cell.rotation) << 16;
                if (!cell.material) continue;
                auto found = std::find_if(palette.begin(), palette.end(), [&](auto const& entry) {
                    return sameMaterial(entry, cell.material);
                });
                auto index = static_cast<std::uint32_t>(found - palette.begin()) + 1;
                if (found == palette.end()) palette.push_back(cell.material);
                references[i] = index | (std::uint32_t(cell.rotation) << 16);
            }
            put32(stream, static_cast<std::uint32_t>(palette.size()));
            for (auto const& material : palette) for (auto pixel : material->pixels()) put32(stream, pixel);
            for (std::size_t i = 0; i < references.size(); ++i) {
                put32(stream, references[i]);
                put32(stream, record.tile.cells[i].tint);
                put32(stream, static_cast<std::uint32_t>(record.tile.cells[i].surfaceId));
                put32(stream, static_cast<std::uint32_t>(record.tile.cells[i].surfaceId >> 32));
            }
            auto const& biomes = record.tile.biomes;
            if (biomes.names.size() > 256) throw std::runtime_error("Invalid biome palette size");
            put32(stream, static_cast<std::uint32_t>(biomes.names.size()));
            for (auto const& name : biomes.names) {
                if (name.empty() || name.size() > 256 || name.find('\0') != std::string::npos)
                    throw std::runtime_error("Invalid biome identifier");
                put32(stream, static_cast<std::uint32_t>(name.size()));
                stream.write(name.data(), name.size());
            }
            for (auto id : biomes.cells) {
                if (id > biomes.names.size()) throw std::runtime_error("Invalid biome palette index");
                put32(stream, id);
            }
        }
        stream.close();
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Replace Wayfinder map");
#else
    std::filesystem::rename(temporary, path);
#endif
}
} // namespace wayfinder
