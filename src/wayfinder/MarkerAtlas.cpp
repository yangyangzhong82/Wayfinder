#include "wayfinder/MarkerAtlas.h"
#include <numbers>
#include <string_view>

namespace wayfinder {
namespace {
// Pin, home, portal and skull, in the persisted markerIcons order. These masks
// are rasterized only on upload, never while drawing individual waypoints.
constexpr std::array<std::array<std::string_view, 9>, 4> glyphs{
    {
     {{"...###...",
          "..#####..",
          ".##...##.",
          ".##.+.##.",
          ".#######.",
          "..#####..",
          "...###...",
          "....#....",
          "....#...."}},
     {{"....#....",
          "...###...",
          "..#####..",
          ".##...##.",
          "##.....##",
          ".#######.",
          ".##...##.",
          ".##...##.",
          ".##...##."}},
     {{"..#####..",
          ".##...##.",
          ".#..+..#.",
          ".#.+...#.",
          ".#..+..#.",
          ".#...+.#.",
          ".#..+..#.",
          ".##...##.",
          "..#####.."}},
     {{"..#####..",
          ".#######.",
          "#########",
          "##..#..##",
          "##..#..##",
          ".###.###.",
          "..#####..",
          "..#.#.#..",
          "..#####.."}},
     }
};
static_assert(glyphs.size() == markerIcons.size());

std::uint32_t highlight(std::uint32_t color) {
    return rgba(((color & 255u) + 255u) / 2, (((color >> 8) & 255u) + 255u) / 2, (((color >> 16) & 255u) + 255u) / 2);
}
} // namespace

int MarkerAtlas::arrowIcon(double dx, double dy) {
    // Screen coordinates: east = 0, south = 4, west = 8, north = 12.
    auto sector = static_cast<int>(std::round(std::atan2(dy, dx) * directionCount / (2 * std::numbers::pi)));
    return iconCount + (sector + directionCount) % directionCount;
}

std::array<float, 2> MarkerAtlas::uv(int icon, int color, bool target) {
    int column = std::clamp(icon, 0, columns - 1);
    int row    = std::clamp(color, 0, static_cast<int>(markerColors.size()) - 1) * 2 + (target ? 1 : 0);
    return {float(column * tileSize) / width, float(row * tileSize) / height};
}

std::vector<std::uint32_t> MarkerAtlas::pixels() {
    std::vector<std::uint32_t> result(width * height, 0);
    for (int icon = 0; icon < columns; ++icon) {
        double angle  = (icon - iconCount) * (2 * std::numbers::pi / directionCount);
        double cosine = std::cos(angle), sine = std::sin(angle);
        for (int row = 0; row < rows; ++row) {
            auto color = markerColors[row / 2];
            auto light = highlight(color);
            for (int y = 2; y <= 14; ++y) {
                for (int x = 2; x <= 14; ++x) {
                    int           dx = x - 8, dy = y - 8;
                    int           edge   = std::max(std::abs(dx), std::abs(dy));
                    int           corner = std::abs(dx) + std::abs(dy);
                    std::uint32_t pixel{};
                    if (edge <= 5 && corner <= 9) pixel = (rgba(10, 14, 20) & 0x00ffffffu) | 0xd9000000u;
                    else if ((row & 1) && edge <= 6 && corner <= 11) pixel = rgba(255, 255, 255);
                    if (std::abs(dx) <= 4 && std::abs(dy) <= 4) {
                        if (icon < iconCount) {
                            char ink = glyphs[icon][dy + 4][dx + 4];
                            if (ink != '.') pixel = ink == '+' ? light : color;
                        } else {
                            // A filled arrow with a shaft, rotated once into each
                            // of sixteen cached directions (clockwise on screen).
                            double along  = dx * cosine + dy * sine;
                            double across = std::abs(-dx * sine + dy * cosine);
                            if (along >= -3.5 && along <= 4.5
                                && ((along >= -0.5 && across <= (4.5 - along) * 0.75) || (along <= 0.5 && across <= 1)))
                                pixel = color;
                        }
                    }
                    result[(row * tileSize + y) * width + icon * tileSize + x] = pixel;
                }
            }
        }
    }
    // Transparent gutters keep neighboring cells out of bilinear samples.
    return result;
}
} // namespace wayfinder
