// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.09.2026
//
// The uniform blocks of the volumetric fog programs (engine/shaders/slang/programs/
// fog-march.slang, fog-combine.slang, fog-local.slang).
//
#pragma once

#include <cstdint>

namespace visutwin::canvas::volumetric_fog
{
    /**
     * March uniforms. 512 bytes — every member is a float4/float4x4 so the MSL
     * and std140 layouts agree without manual padding. Must match the FogUniforms
     * block declared in both shader sources below.
     */
    struct alignas(16) FogUniforms
    {
        float invView[16];                 // offset   0
        float shadowMatrixPalette[4][16];  // offset  64
        float cameraPosition[4];           // offset 320  xyz
        float cameraForward[4];            // offset 336  xyz
        float projScale[4];                // offset 352  xy
        float tint[4];                     // offset 368  xyz
        float lightColor[4];               // offset 384  xyz
        float lightDirection[4];           // offset 400  xyz
        float ambient[4];                  // offset 416  xyz
        float fogParams[4];                // offset 432  x=density y=heightBase z=heightFalloff w=maxDistance
        float scatterParams[4];            // offset 448  x=anisotropy y=steps z=noiseOffset w=shadowIntensity
        float shadowCascadeDistances[4];   // offset 464
        float shadowParams[4];             // offset 480  x=cascadeCount y=bias z=hasShadows w=shadowDistance
        float cameraParams[4];             // offset 496  x=near y=far z=extinction w=shadow map size
    };
    static_assert(sizeof(FogUniforms) == 512);

    /// Combine uniforms. xy = fog resolution, zw = 1/resolution; cameraParams x=near y=far.
    struct alignas(16) FogCombineUniforms
    {
        float textureSize[4];
        float cameraParams[4];
    };
    static_assert(sizeof(FogCombineUniforms) == 32);

    /**
     * Local light uniforms, one block per light.
     * 352 bytes, every member a float4 / float4x4 so the MSL and std140 layouts agree.
     */
    struct alignas(16) FogLocalUniforms
    {
        float invView[16];          // offset   0
        float lightProjMatrix[16];  // offset  64  a spot: world -> its atlas slot (shadow and cookie)
        float cameraPosition[4];    // offset 128  xyz
        float cameraForward[4];     // offset 144  xyz
        float projScale[4];         // offset 160  xy, z = camera near, w = camera far
        float tint[4];              // offset 176  xyz
        float fogParams[4];         // offset 192  density, height base, height falloff, max distance
        float marchParams[4];       // offset 208  anisotropy, steps, noise offset, extinction
        float lightPosRange[4];     // offset 224  xyz position, w range
        float lightSphere[4];       // offset 240  xyz centre, w radius of the volume marched
        float lightColor[4];        // offset 256  xyz, pre-scaled by intensity and exposure
        float lightDir[4];          // offset 272  xyz spot axis, w 1 spot / 0 omni
        float lightSpot[4];         // offset 288  inner cos, outer cos, shadow intensity, cookie intensity
        float lightAtten[4];        // offset 304  x 1 linear falloff, y cookie channel, z atlas resolution
        float lightAtlas[4];        // offset 320  an omni's atlas rect (x, y, size, edge pixels)
        float omniDepth[4];         // offset 336  an omni's shadow near, far, relative bias
    };
    static_assert(sizeof(FogLocalUniforms) == 352);

}
