// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// glTF parser: image loading and decoding, samplers and texture creation.
#include "glbParserInternal.h"

#include "glbParser.h"
#include "framework/parsers/packedVertex.h"

#include "framework/components/light/lightComponent.h"

#include <algorithm>
#include <atomic>
#include <numbers>
#include <mutex>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "core/shape/boundingBox.h"
#include "core/math/matrix4.h"
#include "core/math/vector4.h"
#include "core/math/vector3.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/textureMips.h"
#include "platform/graphics/textureUtils.h"
#include "platform/graphics/vertexFormat.h"
#include "framework/parsers/texture/ktx2Transcoder.h"
#include "framework/parsers/texture/webpDecoder.h"
#include "scene/gsplat/gsplatResource.h"
#include "scene/materials/standardMaterial.h"
#include "spdlog/spdlog.h"
#include "stb_image.h"
#include "framework/assets/stbImageFlip.h"

namespace visutwin::canvas
{
    using namespace gltf;

    // ── Public image-loader callback ─────────────────────────────────────

    bool GlbParser::loadImageData(tinygltf::Image* image,
        const int imageIndex,
        std::string* err,
        std::string* warn,
        const int reqWidth,
        const int reqHeight,
        const unsigned char* bytes,
        const int size,
        void* userData)
    {
        (void)imageIndex;
        (void)warn;
        (void)reqWidth;
        (void)reqHeight;
        (void)userData;

        if (!image || !bytes || size <= 0) {
            if (err) {
                *err = "Invalid image payload";
            }
            return false;
        }

        // KTX2 (KHR_texture_basisu): keep the raw supercompressed bytes verbatim —
        // they are transcoded to a GPU block-compressed format at texture creation.
        if (Ktx2Transcoder::isKtx2(bytes, static_cast<size_t>(size))) {
            image->width = 0;
            image->height = 0;
            image->component = 0;
            image->bits = 8;
            image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
            image->image.assign(bytes, bytes + size);
            return true;
        }

        // Every other image is kept ENCODED too, marked `as_is` (tinygltf's own flag
        // for "the bytes are the file's, not pixels"), and decoded by prepareFromModel.
        // tinygltf calls this for one image at a time while it parses, so decoding
        // here decodes a model's images one after another on the calling thread, which
        // is most of what loading a textured model costs; prepareFromModel has all of
        // them in hand and decodes them side by side.
        image->width = 0;
        image->height = 0;
        image->component = 0;
        image->bits = 8;
        image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
        image->as_is = true;
        image->image.assign(bytes, bytes + size);
        return true;
    }
    namespace
    {
        FilterMode mapMinFilter(const int minFilter)
        {
            switch (minFilter) {
            case 9728: // NEAREST
                return FilterMode::FILTER_NEAREST;
            case 9729: // LINEAR
                return FilterMode::FILTER_LINEAR;
            case 9984: // NEAREST_MIPMAP_NEAREST
                return FilterMode::FILTER_NEAREST_MIPMAP_NEAREST;
            case 9985: // LINEAR_MIPMAP_NEAREST
                return FilterMode::FILTER_LINEAR_MIPMAP_NEAREST;
            case 9986: // NEAREST_MIPMAP_LINEAR
                return FilterMode::FILTER_NEAREST_MIPMAP_LINEAR;
            case 9987: // LINEAR_MIPMAP_LINEAR
            default:
                return FilterMode::FILTER_LINEAR_MIPMAP_LINEAR;
            }
        }

        FilterMode mapMagFilter(const int magFilter)
        {
            switch (magFilter) {
            case 9728: // NEAREST
                return FilterMode::FILTER_NEAREST;
            case 9729: // LINEAR
            default:
                return FilterMode::FILTER_LINEAR;
            }
        }

        AddressMode mapWrapMode(const int wrapMode)
        {
            switch (wrapMode) {
            case 33071: // CLAMP_TO_EDGE
                return ADDRESS_CLAMP_TO_EDGE;
            case 33648: // MIRRORED_REPEAT
                return ADDRESS_MIRRORED_REPEAT;
            case 10497: // REPEAT
            default:
                return ADDRESS_REPEAT;
            }
        }
    }

    namespace gltf
    {
        bool buildRgba8Image(const tinygltf::Image& image, std::vector<uint8_t>& outRgba)
        {
            if (image.width <= 0 || image.height <= 0 || image.image.empty()) {
                return false;
            }
            if (image.bits != 8 || image.pixel_type != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                return false;
            }
            if (image.component < 1 || image.component > 4) {
                return false;
            }

            const auto pixelCount = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
            outRgba.resize(pixelCount * 4);

            for (size_t i = 0; i < pixelCount; ++i) {
                const auto srcOffset = i * static_cast<size_t>(image.component);
                const auto dstOffset = i * 4;
                const auto* src = image.image.data() + srcOffset;
                auto* dst = outRgba.data() + dstOffset;
                switch (image.component) {
                case 1:
                    dst[0] = src[0];
                    dst[1] = src[0];
                    dst[2] = src[0];
                    dst[3] = 255;
                    break;
                case 2:
                    dst[0] = src[0];
                    dst[1] = src[0];
                    dst[2] = src[0];
                    dst[3] = src[1];
                    break;
                case 3:
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = 255;
                    break;
                case 4:
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = src[3];
                    break;
                default:
                    return false;
                }
            }

            return true;
        }

        /// True when a glTF image holds raw KTX2 bytes (stored verbatim by loadImageData
        /// for KHR_texture_basisu payloads).
        bool imageHoldsKtx2(const tinygltf::Image& image)
        {
            return Ktx2Transcoder::isKtx2(image.image.data(), image.image.size());
        }

        /// Decodes an image loadImageData left encoded (`as_is`) straight to RGBA8, with
        /// row 0 at the top as every texture here has it: a WebP through libwebp, every other
        /// format through stb. An image neither can read (a .basis payload, say) becomes a 1x1
        /// magenta placeholder so the model's geometry still loads. Safe on any thread: the flip
        /// is per thread.
        void decodeEncodedImage(const tinygltf::Image& image, const size_t imageIndex,
            PreparedGlbData::ImageData& out)
        {
            int width = 0;
            int height = 0;
            int components = 0;
            if (isWebP(image.image.data(), image.image.size())) {
                if (!decodeWebP(image.image.data(), image.image.size(), out.rgbaPixels, width, height)) {
                    spdlog::warn("GLB image #{}: libwebp cannot decode, mimeType={} — using placeholder",
                        imageIndex, image.mimeType);
                    out.rgbaPixels = {255, 0, 255, 255};
                    out.width = 1;
                    out.height = 1;
                    out.valid = true;
                    return;
                }
                out.width = width;
                out.height = height;
                out.valid = true;
                return;
            }
            stbi_uc* decoded = nullptr;
            {
                // Row 0 at the top, as the file stores it and as glTF's UVs address it.
                // The flip state is per thread and restored afterwards (stbImageFlip.h).
                const StbVerticalFlipScope flipScope(false);
                decoded = stbi_load_from_memory(image.image.data(), static_cast<int>(image.image.size()),
                    &width, &height, &components, 4);
            }
            if (!decoded) {
                const char* reason = stbi_failure_reason();
                spdlog::warn("GLB image #{}: stb_image cannot decode ({}), mimeType={} — using placeholder",
                    imageIndex, reason ? reason : "unknown", image.mimeType);
                out.rgbaPixels = {255, 0, 255, 255};
                out.width = 1;
                out.height = 1;
                out.valid = true;
                return;
            }
            const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
            out.rgbaPixels.assign(decoded, decoded + byteCount);
            stbi_image_free(decoded);
            out.width = width;
            out.height = height;
            out.valid = true;
        }

        // The image a texture samples: its `source`, or the `source` of one of the
        // extensions that store it there instead.
        int textureImageSource(const tinygltf::Texture& texture)
        {
            if (texture.source >= 0) {
                return texture.source;
            }
            static const char* textureExtensions[] = {
                "KHR_texture_basisu", "EXT_texture_webp", "EXT_texture_avif", "MSFT_texture_dds"
            };
            for (const auto* extName : textureExtensions) {
                const auto it = texture.extensions.find(extName);
                if (it != texture.extensions.end() && it->second.IsObject()) {
                    const auto sourceVal = it->second.Get("source");
                    if (sourceVal.IsInt()) {
                        return sourceVal.GetNumberAsInt();
                    }
                }
            }
            return -1;
        }

        GltfSamplerState resolveGltfSampler(const tinygltf::Model& model, const tinygltf::Texture& srcTexture)
        {
            GltfSamplerState state;
            if (srcTexture.sampler < 0 || srcTexture.sampler >= static_cast<int>(model.samplers.size())) {
                return state;
            }
            const auto& sampler = model.samplers[static_cast<size_t>(srcTexture.sampler)];
            if (sampler.minFilter != -1) {
                auto minFilter = mapMinFilter(sampler.minFilter);
                if (minFilter == FilterMode::FILTER_NEAREST_MIPMAP_NEAREST ||
                    minFilter == FilterMode::FILTER_LINEAR_MIPMAP_NEAREST ||
                    minFilter == FilterMode::FILTER_NEAREST_MIPMAP_LINEAR ||
                    minFilter == FilterMode::FILTER_LINEAR_MIPMAP_LINEAR) {
                    minFilter = FilterMode::FILTER_LINEAR;
                }
                state.minFilter = static_cast<int>(minFilter);
            }
            if (sampler.magFilter != -1) {
                state.magFilter = static_cast<int>(mapMagFilter(sampler.magFilter));
            }
            state.addressU = mapWrapMode(sampler.wrapS);
            state.addressV = mapWrapMode(sampler.wrapT);
            return state;
        }

        void applyGltfSampler(Texture& texture, const GltfSamplerState& state)
        {
            if (state.minFilter != -1) {
                texture.setMinFilter(static_cast<FilterMode>(state.minFilter));
            }
            if (state.magFilter != -1) {
                texture.setMagFilter(static_cast<FilterMode>(state.magFilter));
            }
            texture.setAddressU(state.addressU);
            texture.setAddressV(state.addressV);
        }

        // Every texture a material of the model reads as sRGB colour, by texture index: base
        // colour, emissive, sheen colour, specular colour, the spec-gloss diffuse and specular,
        // and the diffuse transmission colour.
        std::vector<bool> gltfColourTextures(const tinygltf::Model& model)
        {
            std::vector<bool> colour(model.textures.size(), false);
            const auto mark = [&colour](const int index) {
                if (index >= 0 && index < static_cast<int>(colour.size())) {
                    colour[static_cast<size_t>(index)] = true;
                }
            };
            const auto markExtension = [&mark](const tinygltf::Material& material, const char* extension,
                const char* texture) {
                const auto it = material.extensions.find(extension);
                if (it != material.extensions.end() && it->second.IsObject() && it->second.Has(texture)) {
                    const auto& info = it->second.Get(texture);
                    if (info.IsObject() && info.Has("index") && info.Get("index").IsInt()) {
                        mark(info.Get("index").GetNumberAsInt());
                    }
                }
            };
            for (const auto& material : model.materials) {
                mark(material.pbrMetallicRoughness.baseColorTexture.index);
                mark(material.emissiveTexture.index);
                markExtension(material, "KHR_materials_sheen", "sheenColorTexture");
                markExtension(material, "KHR_materials_specular", "specularColorTexture");
                markExtension(material, "KHR_materials_pbrSpecularGlossiness", "diffuseTexture");
                markExtension(material, "KHR_materials_pbrSpecularGlossiness", "specularGlossinessTexture");
                markExtension(material, "KHR_materials_diffuse_transmission", "diffuseTransmissionColorTexture");
            }
            return colour;
        }

        std::shared_ptr<Texture> createPreparedTexture(const PreparedGlbData::ImageData& image,
            const std::string& name, const std::shared_ptr<GraphicsDevice>& device)
        {
            TextureOptions options;
            options.profilerHint = TexHint::TEXHINT_ASSET;
            options.width = static_cast<uint32_t>(image.width);
            options.height = static_cast<uint32_t>(image.height);
            if (image.isCompressed) {
                // KHR_texture_basisu: pre-transcoded block-compressed levels.
                options.format = static_cast<PixelFormat>(image.compressedFormat);
                options.mipmaps = image.compressedLevels.size() > 1;
                options.numLevels = static_cast<uint32_t>(image.compressedLevels.size());
                options.minFilter = options.mipmaps ? FilterMode::FILTER_LINEAR_MIPMAP_LINEAR
                                                    : FilterMode::FILTER_LINEAR;
            } else {
                options.format = PixelFormat::PIXELFORMAT_RGBA8;
                // A full mip chain, built with the pixels in prepareFromModel (Texture::srgb).
                // Without mipmaps, trilinear/anisotropic sampling can't minify ground/wall
                // textures at glancing angles and we get radial streak aliasing from the
                // viewer's nadir point.
                options.mipmaps = true;
                options.numLevels = 0;  // 0 = allocate full mip chain based on max(w,h)
                options.minFilter = FilterMode::FILTER_LINEAR_MIPMAP_LINEAR;
            }
            options.magFilter = FilterMode::FILTER_LINEAR;
            options.name = name;

            auto texture = std::make_shared<Texture>(device.get(), options);
            if (image.isCompressed) {
                for (size_t level = 0; level < image.compressedLevels.size(); ++level) {
                    texture->setLevelData(static_cast<uint32_t>(level),
                        image.compressedLevels[level].data(), image.compressedLevels[level].size());
                }
            } else {
                texture->setLevelData(0, image.rgbaPixels.data(), image.rgbaPixels.size());
                for (size_t level = 0; level < image.mipLevels.size(); ++level) {
                    texture->setLevelData(static_cast<uint32_t>(level + 1), image.mipLevels[level].data(),
                        image.mipLevels[level].size());
                }
            }
            texture->setSrgb(image.srgb);
            return texture;
        }
    }
}
