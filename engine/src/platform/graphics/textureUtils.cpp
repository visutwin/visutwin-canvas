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
}
