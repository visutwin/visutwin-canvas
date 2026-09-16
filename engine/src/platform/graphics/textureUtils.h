// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.09.2025.
//

#pragma once

#include <cstddef>
#include <cstdint>

#include "constants.h"

namespace visutwin::canvas
{
    class TextureUtils
    {
    public:
        // Calculate the dimension of a texture at a specific mip level
        static uint32_t calcLevelDimension(uint32_t dimension, uint32_t mipLevel);

        // Calculate the number of mip levels for a texture with the specified dimensionss
        static uint32_t calcMipLevelsCount(uint32_t width, uint32_t height, uint32_t depth = 1);

        /**
         * Bytes ONE mip level of the given dimensions occupies, for a single face or
         * array slice.
         *
         * Block-compressed formats are sized as whole blocks —
         * `ceil(w / blockWidth) * ceil(h / blockHeight) * blockSize` — which is the
         * convention both upload paths already use (`metalTexture.cpp`,
         * `vulkanTexture.cpp`). Bytes-per-pixel is the wrong model for them and
         * `pixelFormatBytesPerPixel` deliberately answers 0, so a caller that forgets
         * the block case measures every compressed texture as free.
         *
         * Returns 0 for a format carrying neither a size nor a block description,
         * which is the same "not described" answer `pixelFormatBytesPerPixel` gives;
         * `tests/pixelFormatTests.cpp` is what keeps the table complete.
         */
        static size_t calcLevelGpuSize(uint32_t width, uint32_t height, uint32_t depth,
            PixelFormat format);

        /**
         * Total GPU bytes a texture occupies: every mip level, multiplied by the six
         * faces of a cubemap and by the array length.
         *
         * `numLevels` is taken from the caller rather than recomputed, because a
         * texture's level count is clamped and can be requested explicitly — deriving
         * it here instead would let the figure added at creation disagree with the one
         * subtracted at release, which is the failure that makes a running total drift
         * rather than simply read wrong.
         *
         * DEVIATION from a strict allocation figure: driver padding, alignment and any
         * backend-internal resolve target are not counted, so this is the content size
         * and a lower bound on what the driver reserved.
         */
        static size_t calcGpuSize(uint32_t width, uint32_t height, uint32_t depth,
            uint32_t numLevels, bool cubemap, uint32_t arrayLength, PixelFormat format);
    };
}
