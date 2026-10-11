#include "wayfinder/MapRenderer.h"
#include "wayfinder/MapMarkers.h"
#include "wayfinder/MapOverlays.h"
#include "wayfinder/SlimeChunks.h"
#include "wayfinder/MarkerAtlas.h"
#include "wayfinder/PortraitPixels.h"
#include "wayfinder/PortraitDraw.h"
#include "wayfinder/WayfinderView.h"
#include <sstream>

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
#include <unordered_map>

namespace wayfinder {
namespace {
RectangleArea rect(float x, float y, float width, float height) { return RectangleArea{x, x + width, y, y + height}; }
void label(MinecraftUIRenderContext& ctx, float x, float y, float width, std::string text, float scale = 0.8f, float opacity = 1.0f,
           mce::Color const& color = mce::Color(235, 241, 247)) {
    TextMeasureData  measure{scale, 0.0f, true, false, false, ui::TextAlignment::Left};
    CaretMeasureData caret{-1, false};
    ctx.drawDebugText(
        rect(x, y, width, 14),
        std::move(text),
        color,
        opacity,
        ui::TextAlignment::Left,
        measure,
        caret
    );
}
// Rough UTF-8 width estimate: ASCII glyphs are ~6px, CJK glyphs ~9px at scale 1.
float textWidth(std::string_view text, float scale) {
    float width = 0;
    for (unsigned char ch : text)
        if ((ch & 0xc0) != 0x80) width += (ch < 0x80 ? 6.0f : 9.0f) * scale;
    return width;
}

// Minimap HUD styling. Rows below the map are contiguous so they read as one panel.
constexpr float hudRowHeight   = 12.0f;
constexpr float hudTextScale   = 0.65f;
mce::Color hudPanelColor() { return mce::Color(12, 17, 23); }
mce::Color hudAccentColor() { return mce::Color(91, 193, 180); }
mce::Color hudTargetColor() { return mce::Color(240, 196, 92); }

// Same order as minimapFrameColorNames; the final "none" entry has no color.
std::optional<mce::Color> minimapFrameColor(int index) {
    switch (index) {
    case 0: return mce::Color(72, 104, 122);  // slate
    case 1: return mce::Color(91, 193, 180);  // teal
    case 2: return mce::Color(222, 178, 86);  // gold
    case 3: return mce::Color(200, 208, 216); // silver
    case 4: return mce::Color(214, 104, 140); // rose
    case 5: return mce::Color(150, 120, 214); // violet
    default: return std::nullopt;
    }
}

void hudRow(MinecraftUIRenderContext& ctx, float x, float y, float width, std::string const& text, mce::Color const& bar,
            float alpha) {
    ctx.setFullClippingRectangle();
    if (alpha > 0) ctx.fillRectangle(rect(x, y, width, hudRowHeight), hudPanelColor(), alpha);
    ctx.fillRectangle(rect(x, y, 2, hudRowHeight), bar, 1.0f);
    ctx.setClippingRectangle(rect(x + 3, y, width - 5, hudRowHeight));
    label(ctx, x + 5, y + 2.5f, width - 7, text, hudTextScale);
    ctx.flushText(0.0f, std::nullopt);
    ctx.setFullClippingRectangle();
}

// Hollow frame built from strips, so a translucent minimap never gets a dark backing.
void ring(MinecraftUIRenderContext& ctx, float x, float y, float width, float height, float thickness,
          mce::Color const& color, float alpha) {
    ctx.fillRectangle(rect(x, y, width, thickness), color, alpha);
    ctx.fillRectangle(rect(x, y + height - thickness, width, thickness), color, alpha);
    ctx.fillRectangle(rect(x, y + thickness, thickness, height - thickness * 2), color, alpha);
    ctx.fillRectangle(rect(x + width - thickness, y + thickness, thickness, height - thickness * 2), color, alpha);
}
} // namespace

struct MapRenderer::Impl {
    ResourceLocation                   location{Core::PathView("wayfinder/runtime/map"), ResourceFileSystem::Raw};
    ResourceLocation                   markerLocation{Core::PathView("wayfinder/runtime/markers"), ResourceFileSystem::Raw};
    ResourceLocation                   circleLocation{Core::PathView("wayfinder/runtime/entity_circles"), ResourceFileSystem::Raw};
    std::shared_ptr<mce::TextureGroup> group;
    std::unique_ptr<mce::TexturePtr>   texture;
    std::unique_ptr<mce::TexturePtr>   markerTexture;
    std::unique_ptr<mce::TexturePtr>   circleTexture;
    std::uint64_t                      revision = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t                      trailRevision{};
    bool                               trailVisible{};
    bool                               slimeVisible{};
    bool                               biomeVisible{};
    int                                width{}, height{};
    // Next free minimap info row; render() fills it so renderMarkers() can append below.
    float                              infoRowY{};
    HashedString                       material{"ui_textured_and_glcolor"};

    struct PortraitTexture {
        ResourceLocation location, displayLocation;
        std::unique_ptr<mce::TexturePtr> source, texture;
        std::shared_ptr<BedrockTextureData const> convertedSource;
        std::chrono::steady_clock::time_point nextRefresh{}, nextConversion{};
        explicit PortraitTexture(std::string_view path)
        : location(Core::PathView(std::string(path)), ResourceFileSystem::UserPackage),
          displayLocation(Core::PathView("wayfinder/runtime/portraits/" + std::string(path)), ResourceFileSystem::Raw) {}
    };
    std::unordered_map<std::string, std::unique_ptr<PortraitTexture>> portraitTextures;

    mce::TexturePtr const* preparePortrait(EntityPortrait const& portrait, int& conversionBudget) {
        auto& slot = portraitTextures[std::string(portrait.texture)];
        if (!slot) slot = std::make_unique<PortraitTexture>(portrait.texture);
        auto now = std::chrono::steady_clock::now();
        if (now >= slot->nextRefresh) {
            // Reacquire periodically so resource-pack reloads replace stale handles.
            // These are shared game textures: never upload over them or unload them.
            slot->nextRefresh = now + std::chrono::seconds(1);
            try {
                slot->source = std::make_unique<mce::TexturePtr>(group->getTexture(
                    slot->location, false, std::nullopt, cg::TextureSetLayerType::Color));
            } catch (std::exception const&) {
                slot->source.reset();
            }
        }
        if (!slot->source || !slot->source->mClientTexture) return nullptr;
        auto const& data = slot->source->mClientTexture;
        if (data->mIsMissingTexture == IsMissingTexture::Yes
            || !(static_cast<unsigned char>(data->mTextureLoadState)
                 & static_cast<unsigned char>(TextureLoadState::LoadedBit))) return nullptr;
        if (slot->convertedSource != data || !slot->texture
            || !group->isLoaded(slot->displayLocation, false, cg::TextureSetLayerType::Color)) {
            if (conversionBudget <= 0 || now < slot->nextConversion) return nullptr;
            --conversionBudget;
            slot->nextConversion = now + std::chrono::seconds(5);
            try {
                auto buffer = group->getCachedImageOrLoadSync(slot->location, false);
                if (!buffer) return nullptr;
                auto const& desc = buffer->mImageDescription;
                auto format = desc->mTextureFormat;
                bool bgra = format == mce::TextureFormat::B8g8r8a8Unorm
                    || format == mce::TextureFormat::B8g8r8a8UnormSrgb;
                int channels = format == mce::TextureFormat::R8g8b8Unorm ? 3 : 4;
                if (!bgra && channels == 4 && format != mce::TextureFormat::R8g8b8a8Unorm
                    && format != mce::TextureFormat::R8g8b8a8UnormSrgb) return nullptr;
                auto const& storage = buffer->mStorage;
                auto pixels = portraitPixels({storage->data(), storage->size()},
                    int(desc->mWidth), int(desc->mHeight), channels, bgra);
                if (pixels.pixels.empty()) return nullptr;
                mce::Image image(pixels.width, pixels.height, mce::ImageFormat::RGBA8Unorm, mce::ImageUsage::SRGB);
                image.resizeImageBytesToFitImageDescription();
                if (image.mImageBytes.size() != pixels.pixels.size() * sizeof(std::uint32_t)) return nullptr;
                std::memcpy(image.mImageBytes.data(), pixels.pixels.data(), image.mImageBytes.size());
                slot->texture.reset();
                if (group->isLoaded(slot->displayLocation, false, cg::TextureSetLayerType::Color))
                    group->unloadTexture(slot->displayLocation, false);
                group->uploadTexture(slot->displayLocation, cg::ImageBuffer(std::move(image)));
                slot->texture = std::make_unique<mce::TexturePtr>(group, slot->displayLocation);
                slot->convertedSource = data;
                slot->nextConversion = {};
            } catch (std::exception const&) {
                return nullptr;
            }
        }
        return slot->texture && slot->texture->mClientTexture ? slot->texture.get() : nullptr;
    }

    bool prepareMarkerAtlas() {
        if (!markerTexture || !group->isLoaded(markerLocation, false, cg::TextureSetLayerType::Color)) {
            auto pixels = MarkerAtlas::pixels();
            mce::Image image(MarkerAtlas::width, MarkerAtlas::height, mce::ImageFormat::RGBA8Unorm, mce::ImageUsage::SRGB);
            image.resizeImageBytesToFitImageDescription();
            if (image.mImageBytes.size() != pixels.size() * sizeof(std::uint32_t)) return false;
            std::memcpy(image.mImageBytes.data(), pixels.data(), image.mImageBytes.size());
            group->uploadTexture(markerLocation, cg::ImageBuffer(std::move(image)));
            markerTexture = std::make_unique<mce::TexturePtr>(group, markerLocation);
        }
        return bool(markerTexture->mClientTexture);
    }
    bool prepareEntityCircles() {
        if (!group) return false;
        if (!circleTexture || !group->isLoaded(circleLocation, false, cg::TextureSetLayerType::Color)) {
            auto pixels = EntityCircleAtlas::pixels();
            mce::Image image(EntityCircleAtlas::width, EntityCircleAtlas::height, mce::ImageFormat::RGBA8Unorm, mce::ImageUsage::SRGB);
            image.resizeImageBytesToFitImageDescription();
            if (image.mImageBytes.size() != pixels.size() * sizeof(std::uint32_t)) return false;
            std::memcpy(image.mImageBytes.data(), pixels.data(), image.mImageBytes.size());
            group->uploadTexture(circleLocation, cg::ImageBuffer(std::move(image)));
            circleTexture = std::make_unique<mce::TexturePtr>(group, circleLocation);
        }
        return bool(circleTexture->mClientTexture);
    }
};

MapRenderer::MapRenderer() : mImpl(std::make_unique<Impl>()) {}
MapRenderer::~MapRenderer() { reset(); }
void MapRenderer::reset() {
    if (mImpl->group) {
        for (auto& [name, portrait] : mImpl->portraitTextures) {
            portrait->texture.reset();
            mImpl->group->unloadTexture(portrait->displayLocation, false);
        }
    }
    mImpl->portraitTextures.clear();
    mImpl->markerTexture.reset();
    mImpl->circleTexture.reset();
    mImpl->texture.reset();
    if (mImpl->group) {
        mImpl->group->unloadTexture(mImpl->markerLocation, false);
        mImpl->group->unloadTexture(mImpl->circleLocation, false);
        mImpl->group->unloadTexture(mImpl->location, false);
    }
    mImpl->group.reset();
    mImpl->revision = std::numeric_limits<std::uint64_t>::max();
    mImpl->width = mImpl->height = 0;
}

bool MapRenderer::render(
    MinecraftUIRenderContext&         ctx,
    MapRect const&                    area,
    MapView const&                    view,
    MapView const&                    textureView,
    std::vector<std::uint32_t> const& pixels,
    BiomeMap const&                   biomes,
    std::uint64_t                     imageRevision,
    double                            playerX,
    double                            playerZ,
    float                             yaw,
    std::string const&                caption,
    bool                              fullscreen,
    Settings const&                   settings,
    Locale const&                     locale,
    bool                              following,
    std::string const&                biome,
    std::string const&                cursorInfo,
    std::string const&                layerLabel,
    ExplorationTrail const&           trail,
    MapLayer                          layer,
    bool                              showPlayer
) {
    mImpl->infoRowY = area.y + area.height + 3;
    if (pixels.size() != static_cast<std::size_t>(textureView.width) * textureView.height) return false;
    auto group = ctx.mClient.getTextureGroup();
    if (!group) return false;
    if (mImpl->group != group) {
        reset();
        mImpl->group = group;
    }
    // Also recover after a resource-pack reload invalidates the texture group entry.
    bool loaded = group->isLoaded(mImpl->location, false, cg::TextureSetLayerType::Color);
    bool slimeVisible = settings.showSlimeChunks
        && slimeOverlayVisible(layer, textureView, view.width * view.blocksPerPixel / area.width);
    if (!loaded || !mImpl->texture || mImpl->revision != imageRevision || mImpl->trailVisible != settings.showTrail
        || mImpl->slimeVisible != slimeVisible
        || mImpl->biomeVisible != settings.showBiomeRegions
        || (settings.showTrail && mImpl->trailRevision != trail.revision)) {
        mce::Image image(
            static_cast<uint32>(textureView.width),
            static_cast<uint32>(textureView.height),
            mce::ImageFormat::RGBA8Unorm,
            mce::ImageUsage::SRGB
        );
        image.resizeImageBytesToFitImageDescription();
        if (image.mImageBytes.size() != pixels.size() * sizeof(std::uint32_t)) return false;
        if (settings.showBiomeRegions || slimeVisible || (settings.showTrail && !trail.points.empty())) {
            auto composited = pixels;
            if (settings.showBiomeRegions) overlayBiomes(composited, textureView, biomes);
            if (slimeVisible) overlaySlimeChunks(composited, textureView, layer);
            if (settings.showTrail) {
                int density = std::clamp(static_cast<int>(std::round(view.blocksPerPixel / textureView.blocksPerPixel)), 1, 4);
                overlayTrail(composited, textureView, layer, trail.points, density);
            }
            std::memcpy(image.mImageBytes.data(), composited.data(), image.mImageBytes.size());
        } else std::memcpy(image.mImageBytes.data(), pixels.data(), image.mImageBytes.size());
        cg::ImageBuffer buffer(std::move(image));
        bool            sameSize = loaded && mImpl->width == textureView.width && mImpl->height == textureView.height;
        if (!sameSize || !group->updateTextureInPlace(mImpl->location, buffer)) {
            group->uploadTexture(mImpl->location, std::move(buffer));
        }
        // Only use documented constructors/fields. No construction, copying, or
        // internal inspection of ClientTexture/ClientResourcePointer takes place here.
        mImpl->texture  = std::make_unique<mce::TexturePtr>(group, mImpl->location);
        mImpl->width    = textureView.width;
        mImpl->height   = textureView.height;
        mImpl->revision = imageRevision;
        mImpl->trailRevision = trail.revision;
        mImpl->trailVisible = settings.showTrail;
        mImpl->slimeVisible = slimeVisible;
        mImpl->biomeVisible = settings.showBiomeRegions;
    }
    auto const& data = mImpl->texture->mClientTexture;
    if (!data) return false;
    ctx.saveCurrentClippingRectangle();
    struct RestoreClip {
        MinecraftUIRenderContext& context;
        ~RestoreClip() { context.restoreSavedClippingRectangle(); }
    } restoreClip{ctx};
    ctx.setFullClippingRectangle();
    // The minimap HUD has no panel fill; only the terrain image uses its opacity.
    if (fullscreen) ctx.fillRectangle(
        rect(area.x - 3, area.y - 14, area.width + 6, area.height + 42),
        mce::Color(12, 17, 23), 0.92f
    );
    // Hold TexturePtr throughout the draw/flush; borrow the engine-owned texture by reference.
    auto uv = textureUvRect(textureView, view);
    ctx.drawImage(data->mClientTexture.get(), {area.x, area.y}, {area.width, area.height},
                  {uv.x, uv.y}, {uv.width, uv.height}, false);
    ctx.flushImages(mce::Color(255, 255, 255), fullscreen ? 1.0f : settings.minimapOpacity, mImpl->material);
    ctx.setClippingRectangle(rect(area.x, area.y, area.width, area.height));
    if (settings.showBiomeRegions && !biomes.cells.empty()) {
        std::vector<MapRect> placed;
        auto origin = view.worldAt(0, 0);
        double blocksPerGuiX = view.width * view.blocksPerPixel / area.width;
        double blocksPerGuiZ = view.height * view.blocksPerPixel / area.height;
        for (auto const& region : biomes.labels) {
            if (placed.size() >= (fullscreen ? 24u : 4u)) break;
            auto name = locale.biome(biomes.names[region.biome - 1]);
            float scale = fullscreen ? 0.75f : 0.6f;
            float width = 6 + textWidth(name, scale);
            float height = 11;
            float x = area.x + float((region.x - origin[0]) / blocksPerGuiX);
            float z = area.y + float((region.z - origin[1]) / blocksPerGuiZ);
            MapRect box{x - width / 2, z - height / 2, width, height};
            if (!area.contains(box.x, box.y) || !area.contains(box.x + box.width, box.y + box.height)) continue;
            if (std::any_of(placed.begin(), placed.end(), [&](auto const& old) { return box.intersects(old); })) continue;
            // Require the full label footprint to remain inside this biome.
            bool fits = true;
            for (int oz : {-1, 0, 1}) for (int ox : {-1, 0, 1})
                if (biomes.atWorld(region.x + ox * width * 0.5 * blocksPerGuiX,
                                   region.z + oz * height * 0.5 * blocksPerGuiZ) != region.biome) fits = false;
            if (!fits) continue;
            ctx.fillRectangle(rect(box.x, box.y, box.width, box.height), mce::Color(12, 17, 23), 0.72f);
            label(ctx, box.x + 3, box.y + 1, width - 6, name, scale);
            placed.push_back(box);
        }
        ctx.flushText(0.0f, std::nullopt);
    }
    if (settings.showChunkBorders) {
        auto origin     = view.worldAt(0, 0);
        auto vertical   = chunkLines(origin[0], view.width * view.blocksPerPixel / area.width, area.width);
        auto horizontal = chunkLines(origin[1], view.height * view.blocksPerPixel / area.height, area.height);
        for (double x : vertical)
            ctx.fillRectangle(rect(area.x + float(x), area.y, 1, area.height), mce::Color(210, 220, 235), 0.35f);
        for (double y : horizontal)
            ctx.fillRectangle(rect(area.x, area.y + float(y), area.width, 1), mce::Color(210, 220, 235), 0.35f);
    }
    ctx.setFullClippingRectangle();
    if (fullscreen) {
        ctx.drawRectangle(rect(area.x, area.y, area.width, area.height), mce::Color(57, 78, 94), 1.0f, 1);
        ctx.setClippingRectangle(rect(area.x, area.y - 13, area.width, 13));
        label(
            ctx,
            area.x + 2,
            area.y - 11,
            area.width,
            "WAYFINDER | " + layerLabel + " | " + locale.tr(following ? "Following" : "Free view")
                + (settings.showSlimeChunks && layer.dimension == 0
                    ? " | " + locale.tr(slimeVisible ? "Slime chunks (green)" : "Slime chunks: zoom in") : ""),
            0.7f
        );
        ctx.flushText(0.0f, std::nullopt);
        ctx.setClippingRectangle(rect(area.x, area.y + area.height, area.width, 14));
        auto info = caption;
        if (settings.showBiome) {
            if (!info.empty()) info += "  |  ";
            info += locale.tr("Player biome: ") + (biome.empty() ? locale.tr("Unknown") : locale.biome(biome));
        }
        label(ctx, area.x + 2, area.y + area.height + 3, area.width - 4, info, 0.65f);
        ctx.flushText(0.0f, std::nullopt);
        ctx.setFullClippingRectangle();
    } else {
        // Soft outer shadow, then a thin two-tone frame that hugs the terrain.
        auto frameColor = minimapFrameColor(settings.minimapFrameColor);
        float infoAlpha = settings.minimapInfoOpacity;
        if (frameColor) {
            ring(ctx, area.x - 3, area.y - 3, area.width + 6, area.height + 6, 1, mce::Color(0, 0, 0), 0.25f);
            ring(ctx, area.x - 2, area.y - 2, area.width + 4, area.height + 4, 2, hudPanelColor(), 0.9f);
            ring(ctx, area.x - 1, area.y - 1, area.width + 2, area.height + 2, 1, *frameColor, 1.0f);
        }
        // Header tab: dimension/layer name sits on a small chip attached to the frame.
        if (!layerLabel.empty()) {
            float tabWidth = std::min(area.width + 4, textWidth(layerLabel, 0.7f) + 12);
            float tabY     = area.y - 15;
            if (infoAlpha > 0) ctx.fillRectangle(rect(area.x - 2, tabY, tabWidth, 13), hudPanelColor(), infoAlpha);
            ctx.fillRectangle(rect(area.x - 2, tabY, tabWidth, 1), frameColor.value_or(hudAccentColor()), 1.0f);
            ctx.setClippingRectangle(rect(area.x, tabY, tabWidth - 4, 13));
            label(ctx, area.x + 3, tabY + 3, tabWidth - 6, layerLabel, 0.7f);
            ctx.flushText(0.0f, std::nullopt);
            ctx.setFullClippingRectangle();
        }
        // Info rows form one contiguous strip under the map.
        float rowY = area.y + area.height + 3;
        if (!caption.empty()) {
            hudRow(ctx, area.x - 2, rowY, area.width + 4, caption, hudAccentColor(), infoAlpha);
            rowY += hudRowHeight;
        }
        if (settings.showBiome) {
            hudRow(ctx, area.x - 2, rowY, area.width + 4,
                locale.tr("Player biome: ") + (biome.empty() ? locale.tr("Unknown") : locale.biome(biome)),
                mce::Color(126, 178, 96), infoAlpha);
            rowY += hudRowHeight;
        }
        mImpl->infoRowY = rowY;
    }

    auto  point = view.worldAt(0, 0);
    float px    = area.x + static_cast<float>((playerX - point[0]) / (view.width * view.blocksPerPixel)) * area.width;
    float py    = area.y + static_cast<float>((playerZ - point[1]) / (view.height * view.blocksPerPixel)) * area.height;
    if (showPlayer && area.contains(px, py)) {
        ctx.setClippingRectangle(rect(area.x, area.y, area.width, area.height));
        float angle = yaw * std::numbers::pi_v<float> / 180.0f;
        float dx = -std::sin(angle), dy = std::cos(angle);
        // Dark outline pass first so the heading stays readable on snow, sand and water.
        for (int pass = 0; pass < 2; ++pass) {
            float pad = pass == 0 ? 1.0f : 0.0f;
            auto  color = pass == 0 ? mce::Color(10, 14, 18) : mce::Color(255, 226, 92);
            float alpha = pass == 0 ? 0.7f : 1.0f;
            for (int i = 2; i < 8; ++i) {
                // Taper from 3px at the body to 1px at the tip.
                float w = (i < 4 ? 3.0f : i < 6 ? 2.0f : 1.0f) + pad * 2;
                float sx = px + dx * i, sy = py + dy * i;
                ctx.fillRectangle(rect(sx - w / 2, sy - w / 2, w, w), color, alpha);
            }
        }
        ctx.fillRectangle(rect(px - 3.5f, py - 3.5f, 7, 7), mce::Color(10, 14, 18), 0.75f);
        ctx.fillRectangle(rect(px - 2.5f, py - 2.5f, 5, 5), mce::Color(255, 255, 255), 1.0f);
        ctx.fillRectangle(rect(px - 1.5f, py - 1.5f, 3, 3), mce::Color(255, 226, 92), 1.0f);
        ctx.setFullClippingRectangle();
    }
    ctx.setClippingRectangle(rect(area.x, area.y, area.width, area.height));
    if (settings.showCompass) {
        // Each letter gets a small chip; north is highlighted for quick orientation.
        struct Cardinal { char const* key; float x, y; bool north; };
        float cx = area.x + area.width * 0.5f, cy = area.y + area.height * 0.5f;
        Cardinal cardinals[] = {
            {"N", cx, area.y + 6, true},
            {"E", area.x + area.width - 6, cy, false},
            {"S", cx, area.y + area.height - 6, false},
            {"W", area.x + 6, cy, false},
        };
        for (auto const& c : cardinals) {
            auto  text = locale.tr(c.key);
            float w = std::max(9.0f, textWidth(text, 0.6f) + 4);
            ctx.fillRectangle(rect(c.x - w / 2, c.y - 5, w, 10), hudPanelColor(), c.north ? 0.85f : 0.6f);
            label(ctx, c.x - w / 2 + 2, c.y - 3.5f, w, text, 0.6f, 1.0f,
                c.north ? mce::Color(255, 112, 96) : mce::Color(220, 228, 236));
        }
    }
    if (settings.showScale && area.height >= 40) {
        auto  scale = mapScale(view, area.width);
        std::ostringstream text;
        text << scale.blocks << locale.tr(" blocks");
        float barWidth = float(scale.pixels);
        float boxWidth = std::max(barWidth, textWidth(text.str(), 0.55f)) + 8;
        // Bottom-left, offset so it does not collide with the compass "S"/"W" chips.
        float x = area.x + 5, y = area.y + area.height - 5;
        // On narrow maps the box would reach the centered "S" chip; lift it above.
        if (settings.showCompass && x - 3 + boxWidth > area.x + area.width * 0.5f - 6) y -= 12;
        ctx.fillRectangle(rect(x - 3, y - 13, boxWidth, 16), hudPanelColor(), 0.55f);
        ctx.fillRectangle(rect(x, y, barWidth + 1, 2), mce::Color(10, 14, 18), 0.8f);
        ctx.fillRectangle(rect(x, y, barWidth, 1), mce::Color(255, 255, 255), 1);
        ctx.fillRectangle(rect(x, y - 3, 1, 4), mce::Color(255, 255, 255), 1);
        ctx.fillRectangle(rect(x + barWidth, y - 3, 1, 4), mce::Color(255, 255, 255), 1);
        label(ctx, x, y - 11, boxWidth - 6, text.str(), 0.55f);
    }
    ctx.flushText(0.0f, std::nullopt);
    ctx.setFullClippingRectangle();
    if (fullscreen) {
        label(
            ctx,
            area.x,
            area.y + area.height + 16,
            area.width,
            cursorInfo.empty() ? locale.tr("Drag: pan | Click marker: edit | Shift+click: navigate | Right click: menu")
                               : cursorInfo,
            0.65f
        );
    }
    ctx.flushText(0.0f, std::nullopt);
    return true;
}
void MapRenderer::renderEntities(
    MinecraftUIRenderContext& ctx, MapRect const& area, MapView const& view,
    std::span<EntityMarker const> entities, MapLayer layer, double playerX, double playerY, double playerZ,
    Settings const& settings, Locale const& locale, bool fullscreen
) {
    auto markers = layoutEntities(entities, view, area, layer, playerX, playerY, playerZ, settings, locale, fullscreen);
    if (markers.empty()) return;
    auto group = ctx.mClient.getTextureGroup();
    if (group && mImpl->group != group) {
        reset();
        mImpl->group = group;
    }
    // Resolve each texture once per pass, retaining its owner through flushImages.
    std::unordered_map<std::string_view, mce::TexturePtr const*> portraits;
    int conversionBudget = 1; // Avoid loading/converting many new types in one HUD frame.
    bool usePortraits = entityPortraitsEnabled(settings, fullscreen);
    bool circlesReady = mImpl->prepareEntityCircles();
    if (usePortraits && group) {
        for (auto const& marker : markers) {
            auto portrait = entityPortrait(marker.type);
            if (portrait && !portraits.contains(portrait->texture))
                portraits.emplace(portrait->texture, mImpl->preparePortrait(*portrait, conversionBudget));
        }
    }
    ctx.saveCurrentClippingRectangle();
    struct Restore {
        MinecraftUIRenderContext& ctx;
        ~Restore() { ctx.restoreSavedClippingRectangle(); }
    } restore{ctx};
    ctx.setClippingRectangle(rect(area.x, area.y, area.width, area.height));
    // Small leaders retain the real map position when crowded portraits fan out.
    // Draw them before heads so a connector never covers another creature's face.
    if (usePortraits) for (auto const& marker : markers) {
        float dx = marker.x - marker.anchorX, dy = marker.y - marker.anchorY;
        float distance = std::max(std::abs(dx), std::abs(dy));
        if (distance < 1) continue;
        int steps = std::min(24, static_cast<int>(std::ceil(distance)));
        for (int step = 0; step < steps; ++step) {
            float t = float(step) / steps;
            ctx.fillRectangle(rect(marker.anchorX + dx*t - 0.5f, marker.anchorY + dy*t - 0.5f, 1, 1),
                mce::Color(210, 220, 230), 0.45f * marker.opacity);
        }
        ctx.fillRectangle(rect(marker.anchorX - 1, marker.anchorY - 1, 2, 2),
            mce::Color(230, 235, 240), 0.7f * marker.opacity);
    }
    // Keep each portrait's source texture and alpha in its own submission.
    // Mixing species in one UI image batch can reuse another species' texture.
    // Draw lower entities first, leaving same-height entities on top.
    std::vector<PlacedEntity const*> circles;
    circles.reserve(markers.size());
    for (bool below : {true, false}) for (bool players : {false, true}) {
        float opacity = below ? settings.undergroundEntityOpacity : 1.0f;
        // Lower entities are dimmed as well as faded, so depth reads at a glance.
        auto tint = below ? mce::Color(150, 150, 150) : mce::Color(255, 255, 255);
        circles.clear();
        for (auto const& marker : markers) {
            if (marker.player != players || marker.below != below) continue;
            float scale = marker.iconScale;
            auto portrait = usePortraits ? entityPortrait(marker.type) : nullptr;
            auto it = portrait ? portraits.find(portrait->texture) : portraits.end();
            if (it != portraits.end() && it->second) {
                float size = entityPortraitSize * scale;
                float left = marker.x - size / 2, top = marker.y - size / 2;
                // 1px dark outline hugging the head's real footprint (JE minimap style).
                float minX = 1, minY = 1, maxX = 0, maxY = 0;
                for (auto const& part : portrait->parts) {
                    minX = std::min(minX, part.x);
                    minY = std::min(minY, part.y);
                    maxX = std::max(maxX, part.x + part.width);
                    maxY = std::max(maxY, part.y + part.height);
                }
                ctx.fillRectangle(rect(left + minX * size - 1, top + minY * size - 1,
                    (maxX - minX) * size + 2, (maxY - minY) * size + 2), mce::Color(18, 22, 28), 0.9f * opacity);
                auto const& data = it->second->mClientTexture;
                submitPortrait(*portrait, [&](std::string_view, PortraitPart const& part) {
                    ctx.drawImage(data->mClientTexture.get(), {left + part.x * size, top + part.y * size},
                        {part.width * size, part.height * size}, {part.u, part.v}, {part.uw, part.vh}, false);
                }, [&] { ctx.flushImages(tint, opacity, mImpl->material); });
                continue;
            }
            circles.push_back(&marker);
        }
        // All colors share one atlas and opacity, so circles need only one submission per group.
        // Finish portrait submissions first to keep their textures out of this batch.
        for (auto marker : circles) {
            int index = EntityCircleAtlas::index(marker->disposition, marker->player);
            float size = 6 * marker->iconScale;
            if (circlesReady) {
                auto const& data = mImpl->circleTexture->mClientTexture;
                ctx.drawImage(data->mClientTexture.get(), {marker->x - size / 2, marker->y - size / 2},
                    {size, size}, {float(index) / EntityCircleAtlas::columns, 0},
                    {1.0f / EntityCircleAtlas::columns, 1}, false);
            } else {
                // Keep round markers visible even if the runtime texture is unavailable.
                auto rgb = EntityCircleAtlas::colors[index];
                float shade = below ? 150.0f / 255 : 1.0f;
                mce::Color color(int((rgb & 255) * shade), int(((rgb >> 8) & 255) * shade), int(((rgb >> 16) & 255) * shade));
                auto disc = [&](float radius, mce::Color const& c) {
                    for (int row = 0; row < 8; ++row) {
                        float y = (row + 0.5f) / 4 - 1;
                        float half = std::sqrt(std::max(0.0f, 1 - y * y)) * radius;
                        ctx.fillRectangle(rect(marker->x - half, marker->y + y * radius - radius / 8, half * 2, radius / 4),
                            c, opacity);
                    }
                };
                // Same solid dot + dark rim as the atlas.
                disc(size / 2, mce::Color(18, 22, 28));
                disc(size / 2 - std::max(0.75f, size / 8), color);
            }
        }
        if (circlesReady && !circles.empty()) ctx.flushImages(tint, opacity, mImpl->material);
    }
    for (bool below : {true, false}) for (auto const& marker : markers) {
        if (marker.count < 2 || marker.below != below) continue;
        auto box = entityCountBounds(marker, settings);
        label(ctx, box.x + 2, box.y, box.width - 3, std::to_string(marker.count), 0.5f, marker.opacity);
    }
    // Player labels draw last so ordinary entity dots and counts cannot cover names.
    for (bool below : {true, false}) for (auto const& marker : markers) {
        if (!marker.player || marker.below != below) continue;
        auto const& box = marker.label;
        label(ctx, box.x + 2, box.y + 1, box.width - 4, marker.name, marker.textScale, marker.opacity);
    }
    ctx.flushText(0.0f, std::nullopt);
}
void MapRenderer::renderMarkers(
    MinecraftUIRenderContext& ctx,
    MapRect const&            area,
    MapView const&            view,
    Navigation const&         navigation,
    MapLayer                  layer,
    int                       dimension,
    double                    playerX,
    double                    playerY,
    double                    playerZ,
    bool                      fullscreen,
    Settings const&           settings,
    Locale const&             locale
) {
    if (navigation.points.empty() || (!settings.showWaypoints && !settings.showNavigation)) return;
    auto group = ctx.mClient.getTextureGroup();
    if (!group) return;
    if (mImpl->group != group) {
        reset();
        mImpl->group = group;
    }
    // Check/upload once per marker pass. Moving, renaming, recoloring or selecting
    // a waypoint only changes its UVs, never the atlas pixels.
    bool atlasReady = mImpl->prepareMarkerAtlas();
    ctx.saveCurrentClippingRectangle();
    struct Restore {
        MinecraftUIRenderContext& ctx;
        ~Restore() { ctx.restoreSavedClippingRectangle(); }
    } restore{ctx};
    ctx.setClippingRectangle(rect(area.x, area.y, area.width, area.height));
    auto markers = layoutMarkers(navigation, view, area, layer.dimension, settings.showWaypoints, settings.showNavigation, fullscreen,
        layer.underground() ? std::optional<int>(layer.referenceY()) : std::nullopt, fullscreen);
    for (float opacity : {0.4f, 1.0f}) {
        bool queuedImages = false;
        for (auto const& marker : markers) {
            if (marker.opacity != opacity) continue;
            auto const& p = *navigation.find(marker.id);
            auto const& position = marker.projection;
            float x = marker.x, y = marker.y;
            if (atlasReady) {
                int icon = position.outside ? MarkerAtlas::arrowIcon(position.dx, position.dy) : p.icon;
                auto uv = MarkerAtlas::uv(icon, p.color, marker.target);
                // Keep each opacity batch separate; targets remain bright and draw last.
                auto const& data = mImpl->markerTexture->mClientTexture;
                ctx.drawImage(data->mClientTexture.get(), {x - 8, y - 8},
                              {float(MarkerAtlas::tileSize), float(MarkerAtlas::tileSize)},
                              {uv[0], uv[1]}, {1.0f / MarkerAtlas::columns, 1.0f / MarkerAtlas::rows}, false);
                queuedImages = true;
            }
        }
        if (queuedImages) ctx.flushImages(mce::Color(255, 255, 255), opacity, mImpl->material);
    }
    auto target = navigation.find(navigation.target);
    for (auto const& marker : markers) {
        if (!marker.label) continue;
        auto const& box = *marker.label;
        ctx.setClippingRectangle(rect(box.x, box.y, box.width, box.height));
        label(ctx, box.x, box.y, box.width, waypointName(*navigation.find(marker.id), locale), 0.65f, marker.opacity);
        ctx.flushText(0.0f, std::nullopt);
    }
    ctx.flushText(0.0f, std::nullopt);
    ctx.setFullClippingRectangle();
    if (target && settings.showNavigation && !fullscreen) {
        // Append to the contiguous info strip started by render().
        hudRow(ctx, area.x - 2, mImpl->infoRowY, area.width + 4,
            targetDescription(*target, dimension, playerX, playerY, playerZ, locale), hudTargetColor(),
            settings.minimapInfoOpacity);
        mImpl->infoRowY += hudRowHeight;
    } else if (target && settings.showNavigation) {
        float y = area.y + area.height + 29;
        ctx.fillRectangle(rect(area.x - 3, y - 1, area.width + 6, 13), mce::Color(12, 17, 23), 0.92f);
        ctx.setClippingRectangle(rect(area.x, y, area.width, 12));
        label(
            ctx,
            area.x + 2,
            y,
            area.width - 4,
            targetDescription(*target, dimension, playerX, playerY, playerZ, locale),
            0.65f
        );
        ctx.flushText(0.0f, std::nullopt);
    }
}
void MapRenderer::renderUi(MinecraftUIRenderContext& ctx, UiFrame const& frame, float mouseX, float mouseY) {
    ctx.saveCurrentClippingRectangle();
    struct Restore {
        MinecraftUIRenderContext& ctx;
        ~Restore() { ctx.restoreSavedClippingRectangle(); }
    } restore{ctx};
    ctx.setFullClippingRectangle();
    if (frame.modal) {
        auto const& backdrop = frame.backdrop;
        ctx.fillRectangle(rect(backdrop.x, backdrop.y, backdrop.width, backdrop.height), mce::Color(5, 10, 16), 0.65f);
        auto const& p = frame.panel;
        ctx.fillRectangle(rect(p.x + 3, p.y + 4, p.width, p.height), mce::Color(0, 0, 0), 0.3f);
        ctx.fillRectangle(rect(p.x, p.y, p.width, p.height), mce::Color(17, 25, 34), 1);
        ctx.drawRectangle(rect(p.x, p.y, p.width, p.height), mce::Color(57, 78, 94), 1, 1);
        ctx.fillRectangle(rect(p.x + 1, p.y + 1, p.width - 2, 2), mce::Color(91, 193, 180), 1);
        float pageWidth = frame.pagination.empty() ? 0 : 35;
        ctx.setClippingRectangle(rect(p.x + 6, p.y + 4, p.width - 12 - pageWidth, 17));
        label(ctx, p.x + 8, p.y + 8, p.width - 16 - pageWidth, frame.title, 0.75f);
        ctx.flushText(0.0f, std::nullopt);
        if (pageWidth > 0) {
            ctx.setClippingRectangle(rect(p.x + p.width - 40, p.y + 4, 34, 17));
            label(ctx, p.x + p.width - 39, p.y + 9, 33, frame.pagination, 0.6f);
            ctx.flushText(0.0f, std::nullopt);
        }
        ctx.setClippingRectangle(rect(p.x + 6, p.y + p.height - 14, p.width - 12, 12));
        label(ctx, p.x + 8, p.y + p.height - 12, p.width - 16, frame.message, 0.55f);
        ctx.flushText(0.0f, std::nullopt);
        ctx.setFullClippingRectangle();
    }
    for (auto const& b : frame.buttons) {
        auto const& r = b.rect;
        bool interactive = b.enabled && b.action != UiAction::None;
        bool hovered = interactive && r.contains(mouseX, mouseY);
        bool selected = b.selected && !b.toggle;
        auto background = hovered ? mce::Color(43, 64, 78)
                        : selected ? mce::Color(35, 64, 72)
                                   : mce::Color(25, 37, 49);
        ctx.fillRectangle(rect(r.x, r.y, r.width, r.height), background, b.enabled ? 1.0f : 0.45f);
        if (hovered || selected)
            ctx.fillRectangle(rect(r.x, r.y + r.height - 1, r.width, 1), mce::Color(91, 193, 180), 1);
        float valueWidth = b.value.empty() ? 0 : b.toggle ? 34.0f : std::min(90.0f, r.width * 0.46f);
        float textWidth = std::max(1.0f, r.width - 10 - valueWidth);
        // Clip each label independently so a long translation never overwrites its value.
        ctx.setClippingRectangle(rect(r.x + 4, r.y + 1, textWidth, r.height - 2));
        label(ctx, r.x + 5, r.y + (r.height - 8) / 2, textWidth, b.text, 0.65f);
        ctx.flushText(0.0f, std::nullopt);
        ctx.setFullClippingRectangle();
        if (!b.value.empty()) {
            float x = r.x + r.width - valueWidth - 4;
            if (b.toggle) {
                ctx.fillRectangle(rect(x, r.y + 4, valueWidth, r.height - 8),
                                  b.selected ? mce::Color(43, 103, 91) : mce::Color(51, 59, 68), 1);
                ctx.fillRectangle(rect(x + 3, r.y + r.height / 2 - 1, 2, 2),
                                  b.selected ? mce::Color(126, 230, 194) : mce::Color(129, 140, 150), 1);
            }
            ctx.setClippingRectangle(rect(x, r.y + 1, valueWidth, r.height - 2));
            label(ctx, x + (b.toggle ? 9 : 2), r.y + (r.height - 8) / 2, valueWidth - 2, b.value, 0.6f);
            ctx.flushText(0.0f, std::nullopt);
            ctx.setFullClippingRectangle();
        }
    }
}
} // namespace wayfinder
