// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// Reprojects an environment (input 0: an equirect or octahedral 2D map, or a cube) into
// an equirect or octahedral rect of the atlas, decoding from gamma and packing RGBP on
// request. `uvMod` applies the seam expansion: the bake adds a border so bilinear taps at
// a rect edge stay inside it. A cube source is its own variant, because a descriptor
// holding a cube view cannot serve a shader that declares a 2D sampler. The cube is
// sampled along the direction UNCHANGED: the atlas stores the source at the direction it
// came from, and the shading lookups negate X when they read it, as the skybox does with
// its cube; flipping here too would mirror every reflection against the sky.
//
// @variant 2d: SRC_2D=1
// @variant cube: SRC_CUBE=1
import vtquad;
import vtenv;
#include "bindings.slang"

struct ReprojectUniforms
{
    float4 uvMod;
    uint sourceProjection;
    uint encodeRgbp;
    uint decodeSrgb;
    uint targetProjection;
};
VT_DECLARE_QUAD_UNIFORMS(ReprojectUniforms, u)
#if defined(SRC_CUBE)
VT_DECLARE_QUAD_CUBE_INPUT0(sourceTexture)
#else
VT_DECLARE_QUAD_INPUT0(sourceTexture)
#endif

[shader("vertex")]
QuadVarying reprojectVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 reprojectFragment(QuadVarying input) : SV_Target0
{
    const float2 uv = input.uv * u.uvMod.xy + u.uvMod.zw;
    const float3 dir = normalize((u.targetProjection == PROJ_OCTAHEDRAL) ? uvToDirOctahedral(uv) : uvToDirEquirect(uv));
#if defined(SRC_CUBE)
    const float4 raw = sourceTexture.Sample(dir);
#else
    const float2 srcUv = (u.sourceProjection == PROJ_OCTAHEDRAL) ? dirToUvOctahedral(dir) : dirToUvEquirect(dir);
    const float4 raw = sourceTexture.Sample(srcUv);
#endif
    const float3 color = decodeIfSrgb(raw.rgb, u.decodeSrgb != 0u);
    return (u.encodeRgbp != 0u) ? packRgbp(color) : float4(color, 1.0);
}
