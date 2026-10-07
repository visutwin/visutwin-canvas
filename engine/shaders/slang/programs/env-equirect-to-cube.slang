// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// One face of a cube map from an equirect (input 0), decoded from gamma on request. The
// face mapping is the standard cube convention, the one the hardware lookup uses, so the
// cube holds the equirect's texel for every direction: sampling it along d returns
// equirect(d). The reproject and convolve bakes rely on that.
import vtquad;
import vtenv;
#include "bindings.slang"

struct FaceUniforms
{
    uint face;         // 0..5
    uint decodeSrgb;
    uint pad0;
    uint pad1;
};
VT_DECLARE_QUAD_UNIFORMS(FaceUniforms, u)
VT_DECLARE_QUAD_INPUT0(sourceEquirect)

float3 faceUvToDir(uint face, float2 uv)
{
    const float sc = uv.x * 2.0 - 1.0;
    const float tc = uv.y * 2.0 - 1.0;
    float3 dir;
    if (face == 0u) {
        dir = float3(1.0, -tc, -sc);
    } else if (face == 1u) {
        dir = float3(-1.0, -tc, sc);
    } else if (face == 2u) {
        dir = float3(sc, 1.0, tc);
    } else if (face == 3u) {
        dir = float3(sc, -1.0, -tc);
    } else if (face == 4u) {
        dir = float3(sc, -tc, 1.0);
    } else {
        dir = float3(-sc, -tc, -1.0);
    }
    return normalize(dir);
}

[shader("vertex")]
QuadVarying equirectToCubeVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 equirectToCubeFragment(QuadVarying input) : SV_Target0
{
    const float2 uv = clamp(input.uv, float2(0.0, 0.0), float2(1.0, 1.0));
    const float3 dir = faceUvToDir(u.face, uv);
    const float4 raw = sourceEquirect.Sample(dirToUvEquirect(dir));
    return float4(decodeIfSrgb(raw.rgb, u.decodeSrgb != 0u), 1.0);
}
