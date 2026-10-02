// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 06.09.2026
//
// The pixel-format descriptor table (platform/graphics/constants.cpp) is a map the
// enum does not enforce: an enumerator with no entry makes pixelFormatBytesPerPixel()
// return 0, and the Vulkan upload path sizes its staging copy from that. There is no
// way to iterate an enum, so this lists every enumerator by hand: add a format
// here when you add one there, and the test says so if the table lags.

#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "platform/graphics/constants.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

int main()
{
    // Every enumerator. DEPTHSTENCIL is the one format whose byte size is a
    // backend decision (D24S8 or D32S8 by probe) and is never uploaded from the
    // CPU, so it is the one entry allowed to carry no size.
    const std::vector<std::pair<PixelFormat, const char*>> formats = {
        {PixelFormat::PIXELFORMAT_RGB8, "RGB8"},
        {PixelFormat::PIXELFORMAT_RGBA8, "RGBA8"},
        {PixelFormat::PIXELFORMAT_DXT1, "DXT1"},
        {PixelFormat::PIXELFORMAT_DXT3, "DXT3"},
        {PixelFormat::PIXELFORMAT_DXT5, "DXT5"},
        {PixelFormat::PIXELFORMAT_RGBA16F, "RGBA16F"},
        {PixelFormat::PIXELFORMAT_RGBA32F, "RGBA32F"},
        {PixelFormat::PIXELFORMAT_R32F, "R32F"},
        {PixelFormat::PIXELFORMAT_DEPTH, "DEPTH"},
        {PixelFormat::PIXELFORMAT_ASTC_4x4, "ASTC_4x4"},
        {PixelFormat::PIXELFORMAT_ASTC_5x5, "ASTC_5x5"},
        {PixelFormat::PIXELFORMAT_ASTC_6x6, "ASTC_6x6"},
        {PixelFormat::PIXELFORMAT_ASTC_8x8, "ASTC_8x8"},
        {PixelFormat::PIXELFORMAT_ASTC_10x10, "ASTC_10x10"},
        {PixelFormat::PIXELFORMAT_ASTC_12x12, "ASTC_12x12"},
        {PixelFormat::PIXELFORMAT_R8, "R8"},
        {PixelFormat::PIXELFORMAT_RG8, "RG8"},
        {PixelFormat::PIXELFORMAT_BC4, "BC4"},
        {PixelFormat::PIXELFORMAT_BC5, "BC5"},
        {PixelFormat::PIXELFORMAT_BC6H, "BC6H"},
        {PixelFormat::PIXELFORMAT_BC7, "BC7"},
        {PixelFormat::PIXELFORMAT_DEPTH16, "DEPTH16"},
        {PixelFormat::PIXELFORMAT_BGRA8, "BGRA8"},
    };

    std::cout << "pixel format table\n";
    for (const auto& [format, name] : formats) {
        const bool compressed = isCompressedPixelFormat(format);
        const uint32_t bytes = pixelFormatBytesPerPixel(format);
        const bool described = compressed || bytes > 0;
        check(described, std::string(name) + (compressed ? " (block)" : "") + " bytes/pixel " +
            std::to_string(bytes));
    }

    // Three that are easy to leave out of the table (R32F, DEPTH16, BGRA8), by value.
    const auto expect = [&](const PixelFormat f, const uint32_t size, const char* name) {
        const bool ok = pixelFormatBytesPerPixel(f) == size;
        check(ok, std::string(name) + " is " + std::to_string(size) + " bytes");
    };
    expect(PixelFormat::PIXELFORMAT_R32F, 4, "R32F");
    expect(PixelFormat::PIXELFORMAT_DEPTH16, 2, "DEPTH16");
    expect(PixelFormat::PIXELFORMAT_BGRA8, 4, "BGRA8");
    expect(PixelFormat::PIXELFORMAT_RGBA8, 4, "RGBA8");

    return finish("pixel format table");
}
