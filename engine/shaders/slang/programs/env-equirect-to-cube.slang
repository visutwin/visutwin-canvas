// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// One face of a cube map from an equirect (input 0), decoded from gamma on request. The
// face mapping (cubeFaceUvToDir) is the one the hardware lookup uses, so the cube holds the
// equirect's texel for every direction: sampling it along d returns equirect(d). The
// reproject and convolve bakes rely on that.
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

[shader("vertex")]
QuadVarying equirectToCubeVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 equirectToCubeFragment(QuadVarying input) : SV_Target0
{
    const float2 uv = clamp(input.uv, float2(0.0, 0.0), float2(1.0, 1.0));
    const float3 dir = cubeFaceUvToDir(u.face, uv);
    const float4 raw = sourceEquirect.Sample(dirToUvEquirect(dir));
    return float4(decodeIfSrgb(raw.rgb, u.decodeSrgb != 0u), 1.0);
}
