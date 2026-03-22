// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.09.2025.
//

#pragma once

#include <cstdint>

namespace visutwin::canvas
{
    class TextureUtils
    {
    public:
        // Calculate the dimension of a texture at a specific mip level
        static uint32_t calcLevelDimension(uint32_t dimension, uint32_t mipLevel);

        // Calculate the number of mip levels for a texture with the specified dimensionss
        static uint32_t calcMipLevelsCount(uint32_t width, uint32_t height, uint32_t depth = 1);
    };
}
