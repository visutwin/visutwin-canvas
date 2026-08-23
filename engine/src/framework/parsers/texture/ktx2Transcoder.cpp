// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "ktx2Transcoder.h"

#include <algorithm>

#include <spdlog/spdlog.h>

#if defined(VISUTWIN_KTX2_USE_LIBKTX)
#include <ktx.h>
#else
#include <mutex>

#include <basisu/transcoder/basisu_transcoder.h>
#endif

namespace visutwin::canvas
{
    namespace
    {
#if defined(VISUTWIN_KTX2_USE_LIBKTX)
        ktx_transcode_fmt_e toKtxFormat(const PixelFormat format)
        {
            switch (format) {
                case PixelFormat::PIXELFORMAT_BC7:
                    return KTX_TTF_BC7_RGBA;
                case PixelFormat::PIXELFORMAT_DXT1:
                    return KTX_TTF_BC1_RGB;
                case PixelFormat::PIXELFORMAT_DXT5:
                    return KTX_TTF_BC3_RGBA;
                case PixelFormat::PIXELFORMAT_ASTC_4x4:
                default:
                    return KTX_TTF_ASTC_4x4_RGBA;
            }
        }

        /// Owns the ktxTexture2 for the duration of one transcode.
        struct KtxTextureHandle
        {
            ktxTexture2* texture = nullptr;

            ~KtxTextureHandle()
            {
                if (texture) {
                    ktxTexture_Destroy(ktxTexture(texture));
                }
            }
        };

        bool transcodeLevels(const uint8_t* data, const size_t size,
            const std::string& debugName, const PixelFormat targetFormat,
            TranscodedTexture& result)
        {
            KtxTextureHandle handle;
            if (const KTX_error_code err = ktxTexture2_CreateFromMemory(data,
                    static_cast<ktx_size_t>(size), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
                    &handle.texture);
                err != KTX_SUCCESS || !handle.texture) {
                spdlog::error("Ktx2Transcoder [{}]: KTX2 parse failed ({})",
                    debugName, ktxErrorString(err));
                return false;
            }

            ktxTexture2* texture = handle.texture;

            // Only Basis-supercompressed payloads (ETC1S/UASTC) are in scope; a KTX2
            // already holding a concrete format would not match the requested target.
            if (!ktxTexture2_NeedsTranscoding(texture)) {
                spdlog::error("Ktx2Transcoder [{}]: not a Basis Universal payload", debugName);
                return false;
            }

            // A single 2D image per level is all TranscodedTexture can carry.
            if (texture->numLayers > 1 || texture->numFaces > 1 || texture->baseDepth > 1) {
                spdlog::error("Ktx2Transcoder [{}]: texture arrays, cubemaps and 3D "
                              "textures are unsupported ({} layers, {} faces, depth {})",
                    debugName, texture->numLayers, texture->numFaces, texture->baseDepth);
                return false;
            }

            // The component count comes from the data format descriptor, and
            // ktxTexture2_TranscodeBasis replaces that descriptor with the target
            // format's — which is RGBA for every format we target. Sample the source
            // alpha before transcoding, or the answer is always "yes".
            const bool hasAlpha = ktxTexture2_GetNumComponents(texture) == 4;

            if (const KTX_error_code err = ktxTexture2_TranscodeBasis(texture,
                    toKtxFormat(targetFormat), 0);
                err != KTX_SUCCESS) {
                spdlog::error("Ktx2Transcoder [{}]: transcode failed ({})",
                    debugName, ktxErrorString(err));
                return false;
            }

            result.width = texture->baseWidth;
            result.height = texture->baseHeight;
            result.hasAlpha = hasAlpha;

            const uint32_t levelCount = std::max(1u, texture->numLevels);
            const ktx_uint8_t* levelData = ktxTexture_GetData(ktxTexture(texture));
            if (!levelData) {
                spdlog::error("Ktx2Transcoder [{}]: transcoded image data unavailable", debugName);
                return false;
            }

            result.levels.reserve(levelCount);
            for (uint32_t level = 0; level < levelCount; ++level) {
                ktx_size_t offset = 0;
                if (const KTX_error_code err = ktxTexture_GetImageOffset(ktxTexture(texture),
                        level, 0, 0, &offset);
                    err != KTX_SUCCESS) {
                    spdlog::error("Ktx2Transcoder [{}]: level {} offset unavailable ({})",
                        debugName, level, ktxErrorString(err));
                    return false;
                }

                const ktx_size_t levelSize = ktxTexture_GetImageSize(ktxTexture(texture), level);
                result.levels.emplace_back(levelData + offset, levelData + offset + levelSize);
            }
            return true;
        }
#else
        void ensureTranscoderInitialized()
        {
            static std::once_flag initFlag;
            std::call_once(initFlag, [] {
                basist::basisu_transcoder_init();
            });
        }

        basist::transcoder_texture_format toBasisFormat(const PixelFormat format)
        {
            switch (format) {
                case PixelFormat::PIXELFORMAT_BC7:
                    return basist::transcoder_texture_format::cTFBC7_RGBA;
                case PixelFormat::PIXELFORMAT_DXT1:
                    return basist::transcoder_texture_format::cTFBC1_RGB;
                case PixelFormat::PIXELFORMAT_DXT5:
                    return basist::transcoder_texture_format::cTFBC3_RGBA;
                case PixelFormat::PIXELFORMAT_ASTC_4x4:
                default:
                    return basist::transcoder_texture_format::cTFASTC_4x4_RGBA;
            }
        }

        bool transcodeLevels(const uint8_t* data, const size_t size,
            const std::string& debugName, const PixelFormat targetFormat,
            TranscodedTexture& result)
        {
            ensureTranscoderInitialized();

            basist::ktx2_transcoder transcoder;
            if (!transcoder.init(data, static_cast<uint32_t>(size))) {
                spdlog::error("Ktx2Transcoder [{}]: KTX2 parse failed (unsupported layout?)", debugName);
                return false;
            }
            if (!transcoder.start_transcoding()) {
                spdlog::error("Ktx2Transcoder [{}]: start_transcoding failed", debugName);
                return false;
            }

            result.width = transcoder.get_width();
            result.height = transcoder.get_height();
            result.hasAlpha = transcoder.get_has_alpha();

            const uint32_t levelCount = std::max(1u, transcoder.get_levels());
            const auto basisFormat = toBasisFormat(targetFormat);
            const uint32_t bytesPerBlock = basist::basis_get_bytes_per_block_or_pixel(basisFormat);

            result.levels.reserve(levelCount);
            for (uint32_t level = 0; level < levelCount; ++level) {
                basist::ktx2_image_level_info levelInfo{};
                if (!transcoder.get_image_level_info(levelInfo, level, 0, 0)) {
                    spdlog::error("Ktx2Transcoder [{}]: level {} info unavailable", debugName, level);
                    return false;
                }

                std::vector<uint8_t> blocks(static_cast<size_t>(levelInfo.m_total_blocks) * bytesPerBlock);
                if (!transcoder.transcode_image_level(level, 0, 0, blocks.data(),
                        levelInfo.m_total_blocks, basisFormat)) {
                    spdlog::error("Ktx2Transcoder [{}]: transcode failed at level {}", debugName, level);
                    return false;
                }
                result.levels.push_back(std::move(blocks));
            }
            return true;
        }
#endif
    }

    bool Ktx2Transcoder::isKtx2(const uint8_t* data, const size_t size)
    {
        // KTX2 identifier: «KTX 20»\r\n\x1A\n
        static constexpr uint8_t KTX2_IDENTIFIER[12] = {
            0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xAB, 0x0D, 0x0A, 0x1A, 0x0A
        };
        // Byte 7 is 0xBB in the spec; some docs list 0xAB — compare bytes 0-6 and 8-11
        // plus accept either at index 7 to be safe.
        if (!data || size < 12) {
            return false;
        }
        for (int i = 0; i < 12; ++i) {
            if (i == 7) {
                if (data[i] != 0xBB && data[i] != 0xAB) return false;
                continue;
            }
            if (data[i] != KTX2_IDENTIFIER[i]) return false;
        }
        return true;
    }

    TranscodedTexture Ktx2Transcoder::transcode(const uint8_t* data, const size_t size,
        const std::string& debugName, const PixelFormat targetFormat)
    {
        TranscodedTexture result;
        result.format = targetFormat;

        if (!isKtx2(data, size)) {
            spdlog::error("Ktx2Transcoder [{}]: not a KTX2 file", debugName);
            return result;
        }

        if (!transcodeLevels(data, size, debugName, targetFormat, result)) {
            result.levels.clear();
            return result;
        }

        result.valid = true;
        spdlog::info("Ktx2Transcoder [{}]: {}x{}, {} mips → {} ({} KB compressed)",
            debugName, result.width, result.height, result.levels.size(),
            targetFormat == PixelFormat::PIXELFORMAT_ASTC_4x4 ? "ASTC 4x4" : "BC",
            [&result] {
                size_t total = 0;
                for (const auto& lvl : result.levels) total += lvl.size();
                return total / 1024;
            }());
        return result;
    }
}
