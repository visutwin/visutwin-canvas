// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// DdsParser on DDS files built in memory: a DXT1 image with a mip chain (level sizes in whole
// blocks, at least one), a cubemap of six faces, 24-bit BGR widened to RGBA8 in the right channel
// order, 32-bit BGRA reordered, half-float RGBA, and the refusals (a truncated file, an unknown
// FourCC, not a DDS file). The BC decode is checked against blocks whose texels are known:
// DXT1's opaque four-colour and punch-through modes, DXT3's explicit alpha and DXT5's interpolated
// alpha.

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "framework/parsers/texture/ddsParser.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    void put(std::vector<uint8_t>& bytes, const size_t offset, const uint32_t value)
    {
        std::memcpy(bytes.data() + offset, &value, 4);
    }

    // A DDS header: by FourCC when `fourCc` is set, else RGB of `bits` with the red mask given.
    std::vector<uint8_t> header(const uint32_t width, const uint32_t height, const uint32_t mips,
        const uint32_t fourCc, const uint32_t bits = 0, const uint32_t redMask = 0, const bool cubemap = false)
    {
        std::vector<uint8_t> bytes(128, 0);
        put(bytes, 0, 0x20534444u);
        put(bytes, 4, 124);
        put(bytes, 12, height);
        put(bytes, 16, width);
        put(bytes, 28, mips);
        put(bytes, 76, 32);
        put(bytes, 80, fourCc ? 0x4u : 0x40u);
        put(bytes, 84, fourCc);
        put(bytes, 88, bits);
        put(bytes, 92, redMask);
        put(bytes, 112, cubemap ? 0xFE00u : 0u);
        return bytes;
    }

    void append(std::vector<uint8_t>& bytes, const size_t count, const uint8_t value)
    {
        bytes.insert(bytes.end(), count, value);
    }

    constexpr uint32_t kDxt1 = 0x31545844u;
    constexpr uint32_t kDxt5 = 0x35545844u;
}

int main()
{
    std::cout << "DDS files\n";
    {
        // 16 x 8 with 5 levels: 8 blocks, 2, 1, 1, 1 of 8 bytes
        auto bytes = header(16, 8, 5, kDxt1);
        const size_t sizes[] = {64, 16, 8, 8, 8};
        for (size_t s : sizes) {
            append(bytes, s, 0x11);
        }
        const DdsImage image = DdsParser::parse(bytes.data(), bytes.size(), "dxt1");
        bool levelsRight = image.valid && image.levelCount() == 5;
        for (size_t i = 0; levelsRight && i < 5; ++i) {
            levelsRight = image.faces[0][i].size() == sizes[i];
        }
        check(image.valid && image.format == PixelFormat::PIXELFORMAT_DXT1 && image.width == 16 &&
                image.height == 8 && !image.cubemap, "DXT1: the format and size");
        check(levelsRight, "DXT1: every level in whole blocks, at least one");

        auto truncated = bytes;
        truncated.resize(truncated.size() - 1);
        check(!DdsParser::parse(truncated.data(), truncated.size(), "truncated").valid, "a truncated file is refused");
    }
    {
        auto bytes = header(4, 4, 1, kDxt5, 0, 0, true);
        for (int face = 0; face < 6; ++face) {
            append(bytes, 16, static_cast<uint8_t>(face));
        }
        const DdsImage image = DdsParser::parse(bytes.data(), bytes.size(), "cube");
        check(image.valid && image.cubemap && image.faces.size() == 6 && image.faces[5][0][0] == 5,
            "a cubemap has six faces, each its own data");
    }
    {
        // 2 x 1 BGR: blue 10, green 20, red 30, then 40, 50, 60
        auto bytes = header(2, 1, 1, 0, 24, 0x00FF0000u);
        const uint8_t texels[] = {10, 20, 30, 40, 50, 60};
        bytes.insert(bytes.end(), std::begin(texels), std::end(texels));
        const DdsImage image = DdsParser::parse(bytes.data(), bytes.size(), "bgr");
        const std::vector<uint8_t> expected = {30, 20, 10, 255, 60, 50, 40, 255};
        check(image.valid && image.format == PixelFormat::PIXELFORMAT_RGBA8 && image.faces[0][0] == expected,
            "24-bit BGR is widened to RGBA8 in RGBA order");
    }
    {
        auto bytes = header(1, 1, 1, 0, 32, 0x00FF0000u);
        const uint8_t texel[] = {1, 2, 3, 4};
        bytes.insert(bytes.end(), std::begin(texel), std::end(texel));
        const DdsImage image = DdsParser::parse(bytes.data(), bytes.size(), "bgra");
        check(image.valid && image.faces[0][0] == std::vector<uint8_t>{3, 2, 1, 4}, "32-bit BGRA is reordered to RGBA");
    }
    {
        auto bytes = header(2, 2, 1, 113);
        append(bytes, 2 * 2 * 8, 0);
        const DdsImage image = DdsParser::parse(bytes.data(), bytes.size(), "half");
        check(image.valid && image.format == PixelFormat::PIXELFORMAT_RGBA16F && image.faces[0][0].size() == 32,
            "FourCC 113 is half-float RGBA");
        auto unknown = header(4, 4, 1, 0x31435445u);   // "ETC1"
        append(unknown, 8, 0);
        check(!DdsParser::parse(unknown.data(), unknown.size(), "etc1").valid, "an unsupported FourCC is refused");
        const uint8_t png[] = {0x89, 'P', 'N', 'G'};
        check(!DdsParser::isDds(png, sizeof(png)), "not a DDS file");
    }

    std::cout << "\nBC decoding\n";
    {
        // DXT1, colour 0 pure red > colour 1 pure blue: four-colour mode; indices 0, 1, 2, 3 on row 0
        std::vector<uint8_t> block(8, 0);
        const uint16_t red = 0xF800;
        const uint16_t blue = 0x001F;
        std::memcpy(block.data(), &red, 2);
        std::memcpy(block.data() + 2, &blue, 2);
        block[4] = 0b11100100;
        std::vector<uint8_t> rgba;
        check(DdsParser::decodeBlocks(PixelFormat::PIXELFORMAT_DXT1, block.data(), block.size(), 4, 4, rgba),
            "DXT1 decodes");
        check(rgba[0] == 255 && rgba[2] == 0 && rgba[4] == 0 && rgba[6] == 255 && rgba[8] == 170 && rgba[10] == 85 &&
                rgba[12] == 85 && rgba[14] == 170 && rgba[15] == 255,
            "DXT1: the endpoints and the two thirds between them, opaque");
        // colour 0 < colour 1: index 3 is transparent black
        std::memcpy(block.data(), &blue, 2);
        std::memcpy(block.data() + 2, &red, 2);
        block[4] = 0b11;
        DdsParser::decodeBlocks(PixelFormat::PIXELFORMAT_DXT1, block.data(), block.size(), 4, 4, rgba);
        check(rgba[0] == 0 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 0, "DXT1: index 3 punches through");
    }
    {
        // DXT3: alpha nibbles 0..15 across the block, colour white
        std::vector<uint8_t> block(16, 0);
        for (int i = 0; i < 8; ++i) {
            block[i] = static_cast<uint8_t>((2 * i) | ((2 * i + 1) << 4));
        }
        const uint16_t white = 0xFFFF;
        std::memcpy(block.data() + 8, &white, 2);
        std::memcpy(block.data() + 10, &white, 2);
        std::vector<uint8_t> rgba;
        DdsParser::decodeBlocks(PixelFormat::PIXELFORMAT_DXT3, block.data(), block.size(), 4, 4, rgba);
        check(rgba[3] == 0 && rgba[7] == 17 && rgba[63] == 255 && rgba[0] == 255, "DXT3: explicit 4-bit alpha");
    }
    {
        // DXT5: alpha 255 and 0, eight-step mode; texel 0 index 0, texel 1 index 1, texel 2 index 2
        std::vector<uint8_t> block(16, 0);
        block[0] = 255;
        block[1] = 0;
        block[2] = 0b10001000;   // texel 0: 0, texel 1: 1, texel 2: 2 (bits 6-8: 010)
        std::vector<uint8_t> rgba;
        DdsParser::decodeBlocks(PixelFormat::PIXELFORMAT_DXT5, block.data(), block.size(), 4, 4, rgba);
        check(rgba[3] == 255 && rgba[7] == 0 && rgba[11] == (6 * 255) / 7, "DXT5: interpolated alpha");
        std::vector<uint8_t> small;
        check(DdsParser::decodeBlocks(PixelFormat::PIXELFORMAT_DXT5, block.data(), block.size(), 2, 2, small) &&
                small.size() == 16, "a level smaller than a block takes its corner of it");
    }

    return finish("dds parser");
}
