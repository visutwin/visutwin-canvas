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
        bool isInt = false;
    };

    // Information about pixel formats
    static std::unordered_map<PixelFormat, PixelFormatInfo> pixelFormatInfo {
        // float formats
        { PixelFormat::PIXELFORMAT_RGB8, { .size = 4 } },
        { PixelFormat::PIXELFORMAT_RGBA8, { .size = 4 } },
        { PixelFormat::PIXELFORMAT_RGBA16F, { .size = 8 } },
        { PixelFormat::PIXELFORMAT_RGBA32F, { .size = 16 } },
        { PixelFormat::PIXELFORMAT_DEPTHSTENCIL, {} },
        { PixelFormat::PIXELFORMAT_DEPTH, { .size = 4 } },
        { PixelFormat::PIXELFORMAT_R8, { .size = 1 } },
        { PixelFormat::PIXELFORMAT_RG8, { .size = 2 } },
    };

    bool isCompressedPixelFormat(const PixelFormat format)
    {
        return pixelFormatInfo[format].blockSize > 0;
    }

    bool isIntegerPixelFormat(const PixelFormat format) {
        return pixelFormatInfo[format].isInt == true;
    };

}
