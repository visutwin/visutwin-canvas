// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "ktx2Transcoder.h"

#include <mutex>

#include <basisu/transcoder/basisu_transcoder.h>
#include <spdlog/spdlog.h>

namespace visutwin::canvas
{
    namespace
    {
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

        ensureTranscoderInitialized();

        basist::ktx2_transcoder transcoder;
        if (!transcoder.init(data, static_cast<uint32_t>(size))) {
            spdlog::error("Ktx2Transcoder [{}]: KTX2 parse failed (unsupported layout?)", debugName);
            return result;
        }
        if (!transcoder.start_transcoding()) {
            spdlog::error("Ktx2Transcoder [{}]: start_transcoding failed", debugName);
            return result;
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
                return result;
            }

            std::vector<uint8_t> blocks(static_cast<size_t>(levelInfo.m_total_blocks) * bytesPerBlock);
            if (!transcoder.transcode_image_level(level, 0, 0, blocks.data(),
                    levelInfo.m_total_blocks, basisFormat)) {
                spdlog::error("Ktx2Transcoder [{}]: transcode failed at level {}", debugName, level);
                return result;
            }
            result.levels.push_back(std::move(blocks));
        }

        result.valid = true;
        spdlog::info("Ktx2Transcoder [{}]: {}x{}, {} mips → {} ({} KB compressed)",
            debugName, result.width, result.height, levelCount,
            targetFormat == PixelFormat::PIXELFORMAT_ASTC_4x4 ? "ASTC 4x4" : "BC",
            [&result] {
                size_t total = 0;
                for (const auto& lvl : result.levels) total += lvl.size();
                return total / 1024;
            }());
        return result;
    }
}
