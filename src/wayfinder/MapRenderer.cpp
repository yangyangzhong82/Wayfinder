#include "wayfinder/MapRenderer.h"

#include "mc/client/game/IClientInstance.h"
#include "mc/client/gui/CaretMeasureData.h"
#include "mc/client/gui/TextMeasureData.h"
#include "mc/client/renderer/TextureGroup.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/deps/core/image/Image.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/core_graphics/ImageBuffer.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTextureData.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"

#include <chrono>
#include <cstring>
#include <numbers>
#include <string_view>

namespace wayfinder {
namespace {
RectangleArea rect(float x, float y, float width, float height) { return RectangleArea{x, x + width, y, y + height}; }
void label(MinecraftUIRenderContext& ctx, float x, float y, float width, std::string text, float scale = 0.8f) {
    TextMeasureData  measure{scale, 0.0f, true, false, false, ui::TextAlignment::Left};
    CaretMeasureData caret{-1, false};
    ctx.drawDebugText(
        rect(x, y, width, 14),
        std::move(text),
        mce::Color(235, 241, 247),
        1.0f,
        ui::TextAlignment::Left,
        measure,
        caret
    );
}
} // namespace

struct MapRenderer::Impl {
    ResourceLocation                   location{Core::PathView("wayfinder/runtime/map"), ResourceFileSystem::Raw};
    std::shared_ptr<mce::TextureGroup> group;
    std::unique_ptr<mce::TexturePtr>   texture;
    std::uint64_t                      revision = std::numeric_limits<std::uint64_t>::max();
    int                                width{}, height{};
    HashedString                       material{"ui_textured_and_glcolor"};
};

MapRenderer::MapRenderer() : mImpl(std::make_unique<Impl>()) {}
MapRenderer::~MapRenderer() { reset(); }
void MapRenderer::reset() {
    mImpl->texture.reset();
    if (mImpl->group) mImpl->group->unloadTexture(mImpl->location, false);
    mImpl->group.reset();
    mImpl->revision = std::numeric_limits<std::uint64_t>::max();
    mImpl->width = mImpl->height = 0;
}

void MapRenderer::render(
    MinecraftUIRenderContext&         ctx,
    MapRect const&                    area,
    MapView const&                    view,
    std::vector<std::uint32_t> const& pixels,
    std::uint64_t                     imageRevision,
    double                            playerX,
    double                            playerZ,
    float                             yaw,
    std::string const&                caption,
    bool                              fullscreen
) {
    if (pixels.size() != static_cast<std::size_t>(view.width) * view.height) return;
    auto group = ctx.mClient.getTextureGroup();
    if (!group) return;
    if (mImpl->group != group) {
        reset();
        mImpl->group = group;
    }
    // Also recover after a resource-pack reload invalidates the texture group entry.
    bool loaded = group->isLoaded(mImpl->location, false, cg::TextureSetLayerType::Color);
    if (!loaded || !mImpl->texture || mImpl->revision != imageRevision) {
        mce::Image image(
            static_cast<uint32>(view.width),
            static_cast<uint32>(view.height),
            mce::ImageFormat::RGBA8Unorm,
            mce::ImageUsage::SRGB
        );
        image.resizeImageBytesToFitImageDescription();
        if (image.mImageBytes.size() != pixels.size() * sizeof(std::uint32_t)) return;
        std::memcpy(image.mImageBytes.data(), pixels.data(), image.mImageBytes.size());
        cg::ImageBuffer buffer(std::move(image));
        bool            sameSize = loaded && mImpl->width == view.width && mImpl->height == view.height;
        if (!sameSize || !group->updateTextureInPlace(mImpl->location, buffer)) {
            group->uploadTexture(mImpl->location, std::move(buffer));
        }
        // Only use documented constructors/fields. No construction, copying, or
        // internal inspection of ClientTexture/ClientResourcePointer takes place here.
        mImpl->texture  = std::make_unique<mce::TexturePtr>(group, mImpl->location);
        mImpl->width    = view.width;
        mImpl->height   = view.height;
        mImpl->revision = imageRevision;
    }
    auto const& data = mImpl->texture->mClientTexture;
    if (!data) return;
    ctx.saveCurrentClippingRectangle();
    struct RestoreClip {
        MinecraftUIRenderContext& context;
        ~RestoreClip() { context.restoreSavedClippingRectangle(); }
    } restoreClip{ctx};
    ctx.setFullClippingRectangle();
    ctx.fillRectangle(
        rect(area.x - 3, area.y - 14, area.width + 6, area.height + (fullscreen ? 45 : 29)),
        mce::Color(12, 17, 23),
        0.92f
    );
    // Hold TexturePtr throughout the draw/flush; borrow the engine-owned texture by reference.
    ctx.drawImage(data->mClientTexture.get(), {area.x, area.y}, {area.width, area.height}, {0, 0}, {1, 1}, false);
    ctx.flushImages(mce::Color(255, 255, 255), 1.0f, mImpl->material);
    ctx.drawRectangle(rect(area.x, area.y, area.width, area.height), mce::Color(94, 121, 146), 1.0f, 1);
    label(ctx, area.x + 2, area.y - 11, area.width, fullscreen ? "WAYFINDER  |  NORTH UP" : "WAYFINDER  N ^");
    label(ctx, area.x + 2, area.y + area.height + 3, area.width, caption, 0.7f);

    auto  point = view.worldAt(0, 0);
    float px    = area.x + static_cast<float>((playerX - point[0]) / (view.width * view.blocksPerPixel)) * area.width;
    float py    = area.y + static_cast<float>((playerZ - point[1]) / (view.height * view.blocksPerPixel)) * area.height;
    if (area.contains(px, py)) {
        float angle = yaw * std::numbers::pi_v<float> / 180.0f;
        float dx = -std::sin(angle), dy = std::cos(angle);
        for (int i = 0; i < 6; ++i) {
            float sx = px + dx * i, sy = py + dy * i;
            if (area.contains(sx - 1, sy - 1) && area.contains(sx + 1, sy + 1))
                ctx.fillRectangle(rect(sx - 1, sy - 1, 2, 2), mce::Color(255, 238, 112), 1.0f);
        }
        if (area.contains(px - 2, py - 2) && area.contains(px + 2, py + 2))
            ctx.fillRectangle(rect(px - 2, py - 2, 4, 4), mce::Color(255, 255, 255), 1.0f);
    }
    if (fullscreen) {
        label(
            ctx,
            area.x,
            area.y + area.height + 17,
            area.width,
            "Drag / arrows: pan | Wheel / +/-: zoom | Home: follow | F: fit | Esc: close",
            0.65f
        );
    }
    ctx.flushText(0.0f, std::nullopt);
}
void MapRenderer::renderCursor(MinecraftUIRenderContext& ctx, float x, float y, bool dragging) {
    // Draw last, independently of the map texture and of the native HUD cursor policy.
    static constexpr std::string_view rows[] = {
        "#",
        "##",
        "#.#",
        "#..#",
        "#...#",
        "#....#",
        "#.....#",
        "#......#",
        "#.......#",
        "#....####",
        "#..#..#",
        "#.# #..#",
        "##  #..#",
        "#    ##"
    };
    ctx.saveCurrentClippingRectangle();
    struct RestoreClip {
        MinecraftUIRenderContext& context;
        ~RestoreClip() { context.restoreSavedClippingRectangle(); }
    } restoreClip{ctx};
    ctx.setFullClippingRectangle();
    auto fill = dragging ? mce::Color(255, 224, 112) : mce::Color(255, 255, 255);
    x         = std::floor(x);
    y         = std::floor(y);
    for (std::size_t row = 0; row < std::size(rows); ++row) {
        auto line = rows[row];
        for (std::size_t start = 0; start < line.size();) {
            auto end = start + 1;
            while (end < line.size() && line[end] == line[start]) ++end;
            if (line[start] != ' ')
                ctx.fillRectangle(
                    rect(x + start, y + row, static_cast<float>(end - start), 1),
                    line[start] == '#' ? mce::Color(12, 17, 23) : fill,
                    1.0f
                );
            start = end;
        }
    }
}
} // namespace wayfinder
