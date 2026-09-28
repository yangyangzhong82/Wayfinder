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
constexpr char magic[8]       = {'W', 'A', 'Y', 'M', 'A', 'P', '0', '2'};
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

std::vector<TileRecord> readMap(std::filesystem::path const& path, std::string const& identity, std::size_t capacity) {
    if (!std::filesystem::exists(path)) return {};
    std::ifstream       stream(path, std::ios::binary);
    std::array<char, 8> header{};
    stream.read(header.data(), header.size());
    bool legacy = stream && std::equal(header.begin(), header.end(), std::begin(legacyMagic));
    if (!stream || (!legacy && !std::equal(header.begin(), header.end(), std::begin(magic))))
        throw std::runtime_error("Unsupported Wayfinder map format");
    auto length = get32(stream);
    if (length > 4096) throw std::runtime_error("Invalid map identity length");
    std::string savedIdentity(length, char{});
    stream.read(savedIdentity.data(), length);
    if (!stream || savedIdentity != identity) throw std::runtime_error("Map belongs to a different world/profile");
    auto count = get32(stream);
    if (count > 65536) throw std::runtime_error("Map exceeds maximum supported tile count");
    auto expectedSize = 8ull + 4 + length + 4 + static_cast<std::uint64_t>(count) * (12 + 256 * 8);
    if (std::filesystem::file_size(path) != expectedSize) throw std::runtime_error("Map record size mismatch");
    std::vector<TileRecord> records;
    records.reserve(std::min<std::size_t>(count, capacity));
    for (std::uint32_t n = 0; n < count; ++n) {
        TileRecord record{};
        record.key.dimension = std::bit_cast<std::int32_t>(get32(stream));
        record.key.x         = std::bit_cast<std::int32_t>(get32(stream));
        record.key.z         = std::bit_cast<std::int32_t>(get32(stream));
        if (record.key.x < -1875000 || record.key.x > 1875000 || record.key.z < -1875000 || record.key.z > 1875000)
            throw std::runtime_error("Map chunk coordinates outside supported world bounds");
        for (auto& cell : record.tile.cells) {
            cell.color  = get32(stream);
            auto word = get32(stream);
            if (legacy) {
                auto height = std::bit_cast<std::int32_t>(word);
                if (height < -32768 || height > 32767) throw std::runtime_error("Invalid map height");
                cell.height = static_cast<std::int16_t>(height);
            } else {
                // v2: low 16 bits signed height, bits 16-23 water depth, top byte reserved.
                if ((word >> 24) != 0) throw std::runtime_error("Invalid map cell reserved bits");
                cell.height = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(word & 0xffffu));
                cell.depth  = static_cast<std::uint8_t>(word >> 16);
            }
            if (cell.known() && (cell.color >> 24) != 255u) throw std::runtime_error("Invalid map pixel alpha");
        }
        if (records.size() < capacity) records.push_back(std::move(record));
    }
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
            for (auto const& cell : record.tile.cells) {
                put32(stream, cell.color);
                put32(
                    stream,
                    std::bit_cast<std::uint16_t>(cell.height) | (static_cast<std::uint32_t>(cell.depth) << 16)
                );
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
