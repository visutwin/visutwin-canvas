// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.09.2026
//
// The uniform blocks of the environment bakes (engine/shaders/slang/programs/
// env-equirect-to-cube.slang, env-reproject.slang, env-convolve.slang). These run over
// QuadRender inside a beginOfflineWork scope, as the fullscreen effects do.
//
// Layout contract, shared with every other quad effect: the source texture is on
// fragment slot 0 (MSL texture(0) / GLSL set 1 binding 0) and the uniform block
// rides the per-draw material slot (MSL buffer(3) / GLSL set 0 binding 0).
//
#pragma once

#include <cstdint>

namespace visutwin::canvas::env_shaders
{
    /// Uniforms for the equirect-to-cubemap face pass. Scalars only, so MSL and
    /// std140 pack this identically and both shaders can declare one field list.
    struct alignas(16) EquirectToCubeUniforms
    {
        uint32_t face = 0u;        // 0..5, the cube face being rendered
        uint32_t decodeSrgb = 0u;  // decode the source from gamma before writing
        uint32_t _pad0 = 0u;
        uint32_t _pad1 = 0u;
    };
    static_assert(sizeof(EquirectToCubeUniforms) == 16);

    /// Uniforms for the reprojection pass. `uvMod` applies the seam expansion
    /// (the bake adds a border so bilinear taps at a rect edge stay inside it);
    /// the projection ids mirror TextureProjection in platform/graphics/constants.h.
    struct alignas(16) ReprojectUniforms
    {
        float uvMod[4] = {1.0f, 1.0f, 0.0f, 0.0f};
        uint32_t sourceProjection = 2u;
        uint32_t encodeRgbp = 0u;
        uint32_t decodeSrgb = 0u;
        uint32_t targetProjection = 2u;
    };
    static_assert(sizeof(ReprojectUniforms) == 32);

    /// Uniforms for the importance-sampled convolution.
    struct alignas(16) ConvolveUniforms
    {
        float uvMod[4] = {1.0f, 1.0f, 0.0f, 0.0f};
        uint32_t encodeRgbp = 0u;
        uint32_t decodeSrgb = 0u;
        uint32_t numSamples = 0u;
        uint32_t weightByNoL = 0u;
    };
    static_assert(sizeof(ConvolveUniforms) == 32);

}
