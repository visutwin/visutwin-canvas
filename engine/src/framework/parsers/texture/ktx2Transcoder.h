// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "platform/graphics/constants.h"

namespace visutwin::canvas
{
    /**
     * CPU-side result of transcoding a KTX2 (Basis Universal) file: one blob of
     * block-compressed data per mip level, ready for Texture::setLevelData().
     */
    struct TranscodedTexture
    {
        bool valid = false;
        PixelFormat format = PixelFormat::PIXELFORMAT_ASTC_4x4;
        uint32_t width = 0;
        uint32_t height = 0;
        bool hasAlpha = false;
        std::vector<std::vector<uint8_t>> levels;  ///< One entry per mip level.
    };

    /**
     * KTX2 (Basis Universal supercompressed) texture transcoding via the basisu
     * transcoder. Supports ETC1S (BasisLZ) and UASTC payloads and transcodes to
     * ASTC 4x4 — the native compressed format on Apple GPUs (BC7 available as an
     * explicit target for desktop-class fallbacks).
     *
     * Safe to call from the background loader thread (transcoder tables are
     * initialized once, thread-safely).
     */
    class Ktx2Transcoder
    {
    public:
        /** True when the byte stream starts with the KTX2 identifier. */
        static bool isKtx2(const uint8_t* data, size_t size);

        /** Transcode every mip level. Target defaults to ASTC 4x4. */
        static TranscodedTexture transcode(const uint8_t* data, size_t size,
            const std::string& debugName,
            PixelFormat targetFormat = PixelFormat::PIXELFORMAT_ASTC_4x4);
    };
}
