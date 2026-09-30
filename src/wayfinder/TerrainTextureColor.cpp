#include "wayfinder/TerrainTextureColor.h"
#include "mc/client/renderer/TextureGroup.h"
#include "mc/client/renderer/block/BlockGraphics.h"
#include "mc/client/renderer/block/declarative_block_tessellation/TextureSlot.h"
#include "mc/client/renderer/texture/TextureUVCoordinateSet.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/deps/core_graphics/ImageBuffer.h"
#include "mc/deps/nbt/StringTag.h"

namespace wayfinder {
void TerrainTextureColors::reset() {
    mGroup = nullptr;
    mMaterials.clear();
    mFaces.clear();
    mPool = {};
    mTicks = 0;
}
void TerrainTextureColors::beginTick(mce::TextureGroup* group) {
    if (mGroup != group || ++mTicks >= 1200) {
        mMaterials.clear();
        mFaces.clear();
        mGroup = group;
        mTicks = 0;
    }
    mNewTextures = 1;
}
TerrainTextureColors::Top TerrainTextureColors::top(Block const& block, BlockPos const& pos) {
    if (!mGroup) return {};
    try {
        auto graphics = BlockGraphics::getForBlock(block);
        if (!graphics) return {};
        if (mFaces.size() >= 4096) mFaces.clear();
        auto [face, inserted] = mFaces.try_emplace(&block);
        if (inserted) {
            face->second.variant = block.getBlockType().getVariant(block);
            auto states = block.mSerializationId->get("states");
            if (states && states->getId() == Tag::Type::Compound) {
                auto const& values = states->as<CompoundTag>();
                auto text = [&](std::string_view key) -> std::string_view {
                    auto value = values.get(key);
                    return value && value->getId() == Tag::Type::String ? std::string_view(value->as<StringTag>()) : std::string_view{};
                };
                auto axis = text("pillar_axis");
                if (axis == "x" || axis == "z") {
                    face->second.slot = static_cast<std::uint64_t>(DeclarativeBlockTessellation::TextureSlot::North);
                    face->second.rotation = axis == "x" ? 1 : 0;
                } else {
                    auto direction = text("minecraft:cardinal_direction");
                    face->second.rotation = direction == "east" ? 1 : direction == "south" ? 2 : direction == "west" ? 3 : 0;
                }
            }
        }
        auto const& uv = graphics->getTexture(pos, face->second.slot, face->second.variant);
        auto rotation = face->second.rotation;
        // The UV selection can vary by position. Cache the selected region, not
        // just Block*, otherwise all texture variants become the first sample.
        if (auto it = mMaterials.find(&uv); it != mMaterials.end()) return {it->second, rotation};
        if (!mNewTextures) return {};
        --mNewTextures;
        if (mMaterials.size() >= 4096) mMaterials.clear();
        auto buffer = mGroup->getCachedImageOrLoadAsync(*uv.sourceFileLocation);
        if (!buffer) return {};
        auto const& description = buffer->mImageDescription;
        auto format = description->mTextureFormat;
        bool bgra = format == mce::TextureFormat::B8g8r8a8Unorm
            || format == mce::TextureFormat::B8g8r8a8UnormSrgb;
        int channels = format == mce::TextureFormat::R8g8b8Unorm ? 3 : 4;
        if (!bgra && channels == 4 && format != mce::TextureFormat::R8g8b8a8Unorm
            && format != mce::TextureFormat::R8g8b8a8UnormSrgb)
            return {mMaterials.emplace(&uv, nullptr).first->second, rotation};
        auto const& storage = buffer->mStorage;
        float u0 = uv._u0, v0 = uv._v0, u1 = uv._u1, v1 = uv._v1;
        // sourceFileLocation can address the original image rather than the
        // atlas. Atlas UV offsets must not select a tiny patch of that image.
        if (description->mWidth == uv._sourceImageWidth && description->mHeight == uv._sourceImageHeight
            && (description->mWidth != uv._texSizeW || description->mHeight != uv._texSizeH)) {
            auto sourceUv = sourceTextureUv(u0, v0, u1, v1, uv._texSizeW, uv._texSizeH,
                int(description->mWidth), int(description->mHeight));
            u0 = sourceUv[0]; v0 = sourceUv[1]; u1 = sourceUv[2]; v1 = sourceUv[3];
        }
        auto result = sampleTopMaterial({storage->data(), storage->size()},
            int(description->mWidth), int(description->mHeight), channels, bgra,
            u0, v0, u1, v1);
        result = mPool.intern(std::move(result));
        mMaterials.emplace(&uv, result);
        return {std::move(result), rotation};
    } catch (std::exception const&) {
        return {};
    }
}
} // namespace wayfinder
