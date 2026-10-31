// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// The DDS reader: a 128-byte header (the magic, then DDS_HEADER), the pixel format by FourCC or
// by bit masks, then every face's mip chain in turn. Block-compressed levels are sized in 4 x 4
// blocks, at least one per dimension.
//
#include "ddsParser.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr uint32_t kMagic = 0x20534444u;            // "DDS "
        constexpr uint32_t kHeaderBytes = 128;               // magic + DDS_HEADER
        constexpr uint32_t kFourCcFlag = 0x4u;               // DDPF_FOURCC
        constexpr uint32_t kRgbFlag = 0x40u;                 // DDPF_RGB
        constexpr uint32_t kCubemapFlag = 0x200u;            // DDSCAPS2_CUBEMAP
        constexpr uint32_t kFourCcDxt1 = 0x31545844u;        // "DXT1"
        constexpr uint32_t kFourCcDxt3 = 0x33545844u;        // "DXT3"
        constexpr uint32_t kFourCcDxt5 = 0x35545844u;        // "DXT5"
        constexpr uint32_t kFourCcDx10 = 0x30315844u;        // "DX10"
        constexpr uint32_t kFourCcRgba16F = 113u;
        constexpr uint32_t kFourCcRgba32F = 116u;

        uint32_t readU32(const uint8_t* data, const size_t offset)
        {
            uint32_t value = 0;
            std::memcpy(&value, data + offset, sizeof(value));
            return value;
        }

        // What a level of the format takes, and how the file stores a texel.
        enum class Layout { Blocks, Rgba, Bgra, Rgb, Bgr };

        size_t levelBytes(const PixelFormat format, const Layout layout, const uint32_t width, const uint32_t height)
        {
            if (layout == Layout::Blocks) {
                const size_t blocks = static_cast<size_t>(std::max(1u, (width + 3u) / 4u)) *
                    std::max(1u, (height + 3u) / 4u);
                return blocks * compressedPixelFormatBlockSize(format);
            }
            const size_t texels = static_cast<size_t>(width) * height;
            if (layout == Layout::Rgb || layout == Layout::Bgr) {
                return texels * 3u;
            }
            return texels * pixelFormatBytesPerPixel(format);
        }

        // An RGB565 colour as RGB8.
        std::array<uint8_t, 3> unpack565(const uint16_t c)
        {
            const uint32_t r = (c >> 11) & 0x1Fu;
            const uint32_t g = (c >> 5) & 0x3Fu;
            const uint32_t b = c & 0x1Fu;
            return {static_cast<uint8_t>((r << 3) | (r >> 2)), static_cast<uint8_t>((g << 2) | (g >> 4)),
                static_cast<uint8_t>((b << 3) | (b >> 2))};
        }

        // The four colours of a BC1 block's palette; three and transparent black for a DXT1 block
        // whose first colour is not the greater (only where `dxt1` allows it).
        void decodeColorBlock(const uint8_t* block, const bool dxt1, uint8_t out[16][4])
        {
            uint16_t c0 = 0;
            uint16_t c1 = 0;
            std::memcpy(&c0, block, 2);
            std::memcpy(&c1, block + 2, 2);
            const auto a = unpack565(c0);
            const auto b = unpack565(c1);
            uint8_t palette[4][4];
            for (int k = 0; k < 3; ++k) {
                palette[0][k] = a[k];
                palette[1][k] = b[k];
            }
            palette[0][3] = palette[1][3] = 255;
            if (!dxt1 || c0 > c1) {
                for (int k = 0; k < 3; ++k) {
                    palette[2][k] = static_cast<uint8_t>((2 * a[k] + b[k]) / 3);
                    palette[3][k] = static_cast<uint8_t>((a[k] + 2 * b[k]) / 3);
                }
                palette[2][3] = palette[3][3] = 255;
            } else {
                for (int k = 0; k < 3; ++k) {
                    palette[2][k] = static_cast<uint8_t>((a[k] + b[k]) / 2);
                    palette[3][k] = 0;
                }
                palette[2][3] = 255;
                palette[3][3] = 0;
            }
            const uint32_t indices = readU32(block, 4);
            for (int i = 0; i < 16; ++i) {
                std::memcpy(out[i], palette[(indices >> (2 * i)) & 0x3u], 4);
            }
        }
    }

    bool DdsParser::isDds(const uint8_t* data, const size_t size)
    {
        return data && size >= 4 && readU32(data, 0) == kMagic;
    }

    DdsImage DdsParser::parse(const uint8_t* data, const size_t size, const std::string& debugName)
    {
        DdsImage image;
        if (!isDds(data, size) || size < kHeaderBytes) {
            spdlog::error("DDS '{}': not a DDS file", debugName);
            return image;
        }
        const uint32_t height = readU32(data, 12);
        const uint32_t width = readU32(data, 16);
        const uint32_t mipCount = std::max(1u, readU32(data, 28));
        const uint32_t pfFlags = readU32(data, 80);
        const uint32_t fourCc = readU32(data, 84);
        const uint32_t bitCount = readU32(data, 88);
        const uint32_t redMask = readU32(data, 92);
        const uint32_t caps2 = readU32(data, 112);
        if (width == 0 || height == 0) {
            spdlog::error("DDS '{}': an empty image", debugName);
            return image;
        }

        PixelFormat format = PixelFormat::PIXELFORMAT_RGBA8;
        Layout layout = Layout::Rgba;
        if (pfFlags & kFourCcFlag) {
            switch (fourCc) {
            case kFourCcDxt1: format = PixelFormat::PIXELFORMAT_DXT1; layout = Layout::Blocks; break;
            case kFourCcDxt3: format = PixelFormat::PIXELFORMAT_DXT3; layout = Layout::Blocks; break;
            case kFourCcDxt5: format = PixelFormat::PIXELFORMAT_DXT5; layout = Layout::Blocks; break;
            case kFourCcRgba16F: format = PixelFormat::PIXELFORMAT_RGBA16F; break;
            case kFourCcRgba32F: format = PixelFormat::PIXELFORMAT_RGBA32F; break;
            default:
                spdlog::error("DDS '{}': FourCC {:#x}{} is not supported", debugName, fourCc,
                    fourCc == kFourCcDx10 ? " (a DX10 header)" : "");
                return image;
            }
        } else if (pfFlags & kRgbFlag) {
            // The order of the channels is the red mask's place.
            if (bitCount == 32) {
                layout = redMask == 0x000000FFu ? Layout::Rgba : Layout::Bgra;
            } else if (bitCount == 24) {
                layout = redMask == 0x000000FFu ? Layout::Rgb : Layout::Bgr;
            } else {
                spdlog::error("DDS '{}': {}-bit RGB is not supported", debugName, bitCount);
                return image;
            }
        } else {
            spdlog::error("DDS '{}': pixel format flags {:#x} are not supported", debugName, pfFlags);
            return image;
        }

        image.format = format;
        image.width = width;
        image.height = height;
        image.cubemap = (caps2 & kCubemapFlag) != 0;
        const uint32_t faceCount = image.cubemap ? 6u : 1u;
        image.faces.resize(faceCount);

        size_t offset = kHeaderBytes;
        for (uint32_t face = 0; face < faceCount; ++face) {
            for (uint32_t level = 0; level < mipCount; ++level) {
                const uint32_t w = std::max(1u, width >> level);
                const uint32_t h = std::max(1u, height >> level);
                const size_t bytes = levelBytes(format, layout, w, h);
                if (offset + bytes > size) {
                    spdlog::error("DDS '{}': the file ends inside face {} level {}", debugName, face, level);
                    return DdsImage{};
                }
                const uint8_t* src = data + offset;
                std::vector<uint8_t> levelData;
                const size_t texels = static_cast<size_t>(w) * h;
                switch (layout) {
                case Layout::Blocks:
                case Layout::Rgba:
                    levelData.assign(src, src + bytes);
                    break;
                case Layout::Bgra:
                    levelData.resize(texels * 4u);
                    for (size_t i = 0; i < texels; ++i) {
                        levelData[i * 4 + 0] = src[i * 4 + 2];
                        levelData[i * 4 + 1] = src[i * 4 + 1];
                        levelData[i * 4 + 2] = src[i * 4 + 0];
                        levelData[i * 4 + 3] = src[i * 4 + 3];
                    }
                    break;
                case Layout::Rgb:
                case Layout::Bgr:
                    levelData.resize(texels * 4u);
                    for (size_t i = 0; i < texels; ++i) {
                        const bool bgr = layout == Layout::Bgr;
                        levelData[i * 4 + 0] = src[i * 3 + (bgr ? 2 : 0)];
                        levelData[i * 4 + 1] = src[i * 3 + 1];
                        levelData[i * 4 + 2] = src[i * 3 + (bgr ? 0 : 2)];
                        levelData[i * 4 + 3] = 255;
                    }
                    break;
                }
                image.faces[face].push_back(std::move(levelData));
                offset += bytes;
            }
        }
        image.valid = true;
        return image;
    }

    bool DdsParser::decodeBlocks(const PixelFormat format, const uint8_t* blocks, const size_t size,
        const uint32_t width, const uint32_t height, std::vector<uint8_t>& rgba)
    {
        if (format != PixelFormat::PIXELFORMAT_DXT1 && format != PixelFormat::PIXELFORMAT_DXT3 &&
            format != PixelFormat::PIXELFORMAT_DXT5) {
            return false;
        }
        const uint32_t blockBytes = compressedPixelFormatBlockSize(format);
        const uint32_t blocksX = std::max(1u, (width + 3u) / 4u);
        const uint32_t blocksY = std::max(1u, (height + 3u) / 4u);
        if (size < static_cast<size_t>(blocksX) * blocksY * blockBytes) {
            return false;
        }
        rgba.assign(static_cast<size_t>(width) * height * 4u, 0);
        for (uint32_t by = 0; by < blocksY; ++by) {
            for (uint32_t bx = 0; bx < blocksX; ++bx) {
                const uint8_t* block = blocks + (static_cast<size_t>(by) * blocksX + bx) * blockBytes;
                uint8_t texels[16][4];
                const bool dxt1 = format == PixelFormat::PIXELFORMAT_DXT1;
                decodeColorBlock(dxt1 ? block : block + 8, dxt1, texels);
                if (format == PixelFormat::PIXELFORMAT_DXT3) {
                    // 4-bit explicit alpha per texel
                    for (int i = 0; i < 16; ++i) {
                        const uint8_t nibble = (block[i / 2] >> ((i % 2) * 4)) & 0x0Fu;
                        texels[i][3] = static_cast<uint8_t>(nibble * 17);
                    }
                } else if (format == PixelFormat::PIXELFORMAT_DXT5) {
                    // two endpoints and 3-bit interpolation indices
                    const uint32_t a0 = block[0];
                    const uint32_t a1 = block[1];
                    uint8_t alphas[8];
                    alphas[0] = static_cast<uint8_t>(a0);
                    alphas[1] = static_cast<uint8_t>(a1);
                    if (a0 > a1) {
                        for (uint32_t k = 1; k < 7; ++k) {
                            alphas[k + 1] = static_cast<uint8_t>(((7 - k) * a0 + k * a1) / 7);
                        }
                    } else {
                        for (uint32_t k = 1; k < 5; ++k) {
                            alphas[k + 1] = static_cast<uint8_t>(((5 - k) * a0 + k * a1) / 5);
                        }
                        alphas[6] = 0;
                        alphas[7] = 255;
                    }
                    uint64_t bits = 0;
                    for (int k = 0; k < 6; ++k) {
                        bits |= static_cast<uint64_t>(block[2 + k]) << (8 * k);
                    }
                    for (int i = 0; i < 16; ++i) {
                        texels[i][3] = alphas[(bits >> (3 * i)) & 0x7u];
                    }
                }
                for (int i = 0; i < 16; ++i) {
                    const uint32_t x = bx * 4u + static_cast<uint32_t>(i % 4);
                    const uint32_t y = by * 4u + static_cast<uint32_t>(i / 4);
                    if (x < width && y < height) {
                        std::memcpy(&rgba[(static_cast<size_t>(y) * width + x) * 4u], texels[i], 4);
                    }
                }
            }
        }
        return true;
    }
}
