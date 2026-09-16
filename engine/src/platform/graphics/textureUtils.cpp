// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.09.2025.
//
#include "textureUtils.h"

#include <algorithm>

namespace visutwin::canvas
{
    uint32_t TextureUtils::calcLevelDimension(const uint32_t dimension, const uint32_t mipLevel)
    {
        return std::max(dimension >> mipLevel, 1u);
    }

    uint32_t TextureUtils::calcMipLevelsCount(uint32_t width, uint32_t height, uint32_t depth) {
        uint32_t maxDimension = std::max({width, height, depth});
        if (maxDimension == 0) {
            return 1;
        }

        // Calculate log2 of the maximum dimension
        uint32_t levels = 1;
        while (maxDimension > 1) {
            maxDimension >>= 1;
            levels++;
        }

        return levels;
    }

    size_t TextureUtils::calcLevelGpuSize(const uint32_t width, const uint32_t height,
        const uint32_t depth, const PixelFormat format)
    {
        const uint32_t levelWidth = std::max(width, 1u);
        const uint32_t levelHeight = std::max(height, 1u);
        const uint32_t levelDepth = std::max(depth, 1u);

        // Block-compressed first: these carry blockSize and no bytes-per-pixel, so
        // testing size() first would answer 0 and silently measure them as free.
        if (const uint32_t blockSize = compressedPixelFormatBlockSize(format); blockSize > 0) {
            const uint32_t blockWidth = std::max(compressedPixelFormatBlockWidth(format), 1u);
            const uint32_t blockHeight = std::max(compressedPixelFormatBlockHeight(format), 1u);

            // Partial blocks at the edge are stored whole — which is why a 1x1 mip of
            // a 4x4-block format still costs a full block, and why the tail of a mip
            // chain does not shrink toward zero.
            const size_t blocksWide = (levelWidth + blockWidth - 1u) / blockWidth;
            const size_t blocksHigh = (levelHeight + blockHeight - 1u) / blockHeight;

            return blocksWide * blocksHigh * blockSize * levelDepth;
        }

        const uint32_t bytesPerPixel = pixelFormatBytesPerPixel(format);
        if (bytesPerPixel == 0) {
            // Not described by the format table. DEPTHSTENCIL is the one legitimate
            // case (its byte size is a backend probe), so answer zero rather than
            // guess a size that would then have to be unwound on release.
            return 0;
        }

        return static_cast<size_t>(levelWidth) * levelHeight * levelDepth * bytesPerPixel;
    }

    size_t TextureUtils::calcGpuSize(const uint32_t width, const uint32_t height,
        const uint32_t depth, const uint32_t numLevels, const bool cubemap,
        const uint32_t arrayLength, const PixelFormat format)
    {
        // arrayLength 0 means "not an array" (Texture::isArray is `_arrayLength > 0`),
        // so it is one slice, not none.
        const uint32_t slices = std::max(arrayLength, 1u) * (cubemap ? 6u : 1u);
        const uint32_t levels = std::max(numLevels, 1u);

        size_t total = 0;
        for (uint32_t level = 0; level < levels; ++level) {
            // Depth halves per level with width and height: a volume texture's mip
            // chain shrinks in three dimensions. An ARRAY does not — its slice count
            // is fixed — which is why slices multiply outside this loop and depth
            // does not.
            total += calcLevelGpuSize(
                calcLevelDimension(width, level),
                calcLevelDimension(height, level),
                calcLevelDimension(depth, level),
                format);
        }

        return total * slices;
    }
}
