#include "wayfinder/MarkerAtlas.h"
#include <fstream>
#include <iostream>
#include <numbers>
#include <stdexcept>

using namespace wayfinder;

namespace {
void require(bool condition, char const* message) {
    if (!condition) throw std::runtime_error(message);
}
} // namespace

int main(int argc, char** argv) {
    auto pixels = MarkerAtlas::pixels();
    require(pixels.size() == MarkerAtlas::width * MarkerAtlas::height, "Atlas dimensions");
    auto pixel = [&](int icon, int row, int x, int y) {
        return pixels[(row * MarkerAtlas::tileSize + y) * MarkerAtlas::width + icon * MarkerAtlas::tileSize + x];
    };
    for (int icon = 0; icon < MarkerAtlas::columns; ++icon) {
        for (int row = 0; row < MarkerAtlas::rows; ++row) {
            auto uv = MarkerAtlas::uv(icon, row / 2, (row & 1) != 0);
            require(
                std::lround(uv[0] * MarkerAtlas::width) == icon * MarkerAtlas::tileSize
                    && std::lround(uv[1] * MarkerAtlas::height) == row * MarkerAtlas::tileSize,
                "UV cell mapping"
            );
            int ink{};
            for (int y = 0; y < MarkerAtlas::tileSize; ++y) {
                for (int x = 0; x < MarkerAtlas::tileSize; ++x) {
                    auto value = pixel(icon, row, x, y);
                    if (x == 0 || y == 0 || x == MarkerAtlas::tileSize - 1 || y == MarkerAtlas::tileSize - 1)
                        require(value == 0, "Transparent sampling gutter");
                    if (x >= 4 && x <= 12 && y >= 4 && y <= 12) {
                        ink += value == markerColors[row / 2];
                        if (row & 1) require(value == pixel(icon, row - 1, x, y), "Selection preserves glyph");
                    }
                }
            }
            require(ink >= 10, "Visible glyph in every palette/direction cell");
            require(pixel(icon, row, 8, 2) == ((row & 1) ? rgba(255, 255, 255) : 0), "Target outline");
            require((pixel(icon, row, 8, 3) >> 24) == 0xd9, "Translucent backdrop");
        }
    }
    // Keep the four saved icon IDs visually distinct, including the death skull.
    for (int a = 0; a < MarkerAtlas::iconCount; ++a) {
        for (int b = a + 1; b < MarkerAtlas::iconCount; ++b) {
            bool different = false;
            for (int y = 4; y <= 12; ++y)
                for (int x = 4; x <= 12; ++x) different |= pixel(a, 0, x, y) != pixel(b, 0, x, y);
            require(different, "Distinct saved icon designs");
        }
    }
    require(MarkerAtlas::arrowIcon(1, 0) == MarkerAtlas::iconCount, "East arrow");
    require(MarkerAtlas::arrowIcon(0, 1) == MarkerAtlas::iconCount + 4, "South arrow");
    require(MarkerAtlas::arrowIcon(-1, 0) == MarkerAtlas::iconCount + 8, "West arrow");
    require(MarkerAtlas::arrowIcon(0, -1) == MarkerAtlas::iconCount + 12, "North arrow");
    for (int step = -720; step <= 720; ++step) {
        double angle = step * std::numbers::pi / 360;
        int    icon  = MarkerAtlas::arrowIcon(std::cos(angle), std::sin(angle));
        require(icon >= MarkerAtlas::iconCount && icon < MarkerAtlas::columns, "Wrapped direction index");
        double selected = (icon - MarkerAtlas::iconCount) * 2 * std::numbers::pi / MarkerAtlas::directionCount;
        require(
            std::abs(std::remainder(selected - angle, 2 * std::numbers::pi))
                <= std::numbers::pi / MarkerAtlas::directionCount + 1e-12,
            "Nearest arrow direction"
        );
    }
    // Optional diagnostic image, composited over a checkerboard for alpha review.
    if (argc == 2) {
        std::ofstream out(argv[1], std::ios::binary);
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out << "P6\n" << MarkerAtlas::width << ' ' << MarkerAtlas::height << "\n255\n";
        for (int y = 0; y < MarkerAtlas::height; ++y) {
            for (int x = 0; x < MarkerAtlas::width; ++x) {
                auto     value      = pixels[y * MarkerAtlas::width + x];
                unsigned alpha      = value >> 24;
                unsigned background = ((x / 8 + y / 8) & 1) ? 72 : 48;
                for (int channel = 0; channel < 3; ++channel)
                    out.put(
                        static_cast<char>(
                            (((value >> (channel * 8)) & 255u) * alpha + background * (255 - alpha)) / 255
                        )
                    );
            }
        }
    }
    std::cout << "Marker atlas: 320 sprites, palette/alpha/UVs and 1441 direction samples passed.\n";
}
