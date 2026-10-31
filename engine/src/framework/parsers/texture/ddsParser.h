// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
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
     * A DDS file read into level data: per face (6 for a cubemap, else 1) one blob per mip level,
     * ready for Texture::setLevelData().
     */
    struct DdsImage
    {
        bool valid = false;
        PixelFormat format = PixelFormat::PIXELFORMAT_RGBA8;
        uint32_t width = 0;
        uint32_t height = 0;
        bool cubemap = false;
        /// faces[face][level]
        std::vector<std::vector<std::vector<uint8_t>>> faces;
        uint32_t levelCount() const { return faces.empty() ? 0u : static_cast<uint32_t>(faces[0].size()); }
    };

    /**
     * Reads DDS files: DXT1, DXT3 and DXT5 (BC1-3), half and float RGBA (FourCC 113 / 116) and
     * uncompressed 32-bit RGBA / BGRA and 24-bit RGB, with mip levels and cubemaps. A 24-bit RGB
     * image is widened to RGBA8 and BGRA reordered to RGBA. Other FourCCs (ETC1, PVRTC, DX10
     * headers) are refused with an error.
     */
    class DdsParser
    {
    public:
        /** True when the byte stream starts with the DDS magic. */
        static bool isDds(const uint8_t* data, size_t size);

        static DdsImage parse(const uint8_t* data, size_t size, const std::string& debugName);

        /**
         * Decodes a DXT1 / DXT3 / DXT5 level of `width` x `height` to RGBA8, rows top first, for a
         * device that cannot sample the block format. False for any other format.
         */
        static bool decodeBlocks(PixelFormat format, const uint8_t* blocks, size_t size, uint32_t width,
            uint32_t height, std::vector<uint8_t>& rgba);
    };
}
