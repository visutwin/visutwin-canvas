// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.09.2026
//
// The uniform block of the TAA resolve, engine/shaders/slang/programs/taa.slang: depth
// reprojection into the previous frame, Catmull-Rom or bilinear history fetch,
// neighbourhood colour clamping, 5% history blend (100% when the reprojection lands
// offscreen). Textures: 0 source, 1 history, 2 scene depth.
//
#pragma once

#include <cstdint>

namespace visutwin::canvas::taa_shaders
{
    /**
     * Matches the TaaParams block in both shaders. Everything is a mat4 or a
     * vec4, so MSL and std140 agree with no padding to get wrong — the size and
     * the two flags share one vec4 rather than a float2 plus two uints, which is
     * how the Vulkan shader already packed them.
     */
    struct alignas(16) TaaUniforms
    {
        float viewProjectionPrevious[16];  // offset   0
        float viewProjectionInverse[16];   // offset  64
        float jitters[4];                  // offset 128
        float texSizeFlags[4];             // offset 144  xy = size, z = highQuality, w = historyValid
        float cameraParams[4];             // offset 160
    };
    static_assert(sizeof(TaaUniforms) == 176);

}
