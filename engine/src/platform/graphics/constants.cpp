// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.09.2025.
//

#include "constants.h"

#include <unordered_map>

namespace visutwin::canvas
{
    struct PixelFormatInfo
    {
        uint32_t size;
        uint32_t blockSize = 0;
        uint32_t blockWidth = 1;
        uint32_t blockHeight = 1;
        bool isInt = false;
    };

    // Information about pixel formats
    static const std::unordered_map<PixelFormat, PixelFormatInfo> pixelFormatInfo {
        // float formats
        { PixelFormat::PIXELFORMAT_RGB8, { .size = 4 } },
        { PixelFormat::PIXELFORMAT_RGBA8, { .size = 4 } },
        { PixelFormat::PIXELFORMAT_RGBA16F, { .size = 8 } },
        { PixelFormat::PIXELFORMAT_RGBA32F, { .size = 16 } },
        { PixelFormat::PIXELFORMAT_DEPTHSTENCIL, {} },
        { PixelFormat::PIXELFORMAT_DEPTH, { .size = 4 } },
        { PixelFormat::PIXELFORMAT_R8, { .size = 1 } },
        { PixelFormat::PIXELFORMAT_RG8, { .size = 2 } },
        // Block-compressed formats (4x4 blocks; blockSize = bytes per block).
        { PixelFormat::PIXELFORMAT_DXT1, { .size = 0, .blockSize = 8, .blockWidth = 4, .blockHeight = 4 } },
        { PixelFormat::PIXELFORMAT_DXT3, { .size = 0, .blockSize = 16, .blockWidth = 4, .blockHeight = 4 } },
        { PixelFormat::PIXELFORMAT_DXT5, { .size = 0, .blockSize = 16, .blockWidth = 4, .blockHeight = 4 } },
        { PixelFormat::PIXELFORMAT_ASTC_4x4, { .size = 0, .blockSize = 16, .blockWidth = 4, .blockHeight = 4 } },
        { PixelFormat::PIXELFORMAT_ASTC_5x5, { .size = 0, .blockSize = 16, .blockWidth = 5, .blockHeight = 5 } },
        { PixelFormat::PIXELFORMAT_ASTC_6x6, { .size = 0, .blockSize = 16, .blockWidth = 6, .blockHeight = 6 } },
        { PixelFormat::PIXELFORMAT_ASTC_8x8, { .size = 0, .blockSize = 16, .blockWidth = 8, .blockHeight = 8 } },
        { PixelFormat::PIXELFORMAT_ASTC_10x10, { .size = 0, .blockSize = 16, .blockWidth = 10, .blockHeight = 10 } },
        { PixelFormat::PIXELFORMAT_ASTC_12x12, { .size = 0, .blockSize = 16, .blockWidth = 12, .blockHeight = 12 } },
        { PixelFormat::PIXELFORMAT_BC4, { .size = 0, .blockSize = 8, .blockWidth = 4, .blockHeight = 4 } },
        { PixelFormat::PIXELFORMAT_BC5, { .size = 0, .blockSize = 16, .blockWidth = 4, .blockHeight = 4 } },
        { PixelFormat::PIXELFORMAT_BC6H, { .size = 0, .blockSize = 16, .blockWidth = 4, .blockHeight = 4 } },
        { PixelFormat::PIXELFORMAT_BC7, { .size = 0, .blockSize = 16, .blockWidth = 4, .blockHeight = 4 } },
    };

    bool isCompressedPixelFormat(const PixelFormat format)
    {
        const auto it = pixelFormatInfo.find(format);
        return it != pixelFormatInfo.end() && it->second.blockSize > 0;
    }

    bool isIntegerPixelFormat(const PixelFormat format) {
        const auto it = pixelFormatInfo.find(format);
        return it != pixelFormatInfo.end() && it->second.isInt;
    };

    uint32_t pixelFormatBytesPerPixel(const PixelFormat format)
    {
        const auto it = pixelFormatInfo.find(format);
        return it != pixelFormatInfo.end() ? it->second.size : 0;
    }

    uint32_t compressedPixelFormatBlockSize(const PixelFormat format)
    {
        const auto it = pixelFormatInfo.find(format);
        return it != pixelFormatInfo.end() ? it->second.blockSize : 0;
    }

    uint32_t compressedPixelFormatBlockWidth(const PixelFormat format)
    {
        const auto it = pixelFormatInfo.find(format);
        return it != pixelFormatInfo.end() ? it->second.blockWidth : 1;
    }

    uint32_t compressedPixelFormatBlockHeight(const PixelFormat format)
    {
        const auto it = pixelFormatInfo.find(format);
        return it != pixelFormatInfo.end() ? it->second.blockHeight : 1;
    }

}
