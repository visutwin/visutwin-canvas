// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// Copies a light cookie into its rect of the clustered cookie atlas. A 2D cookie is copied
// as is; a cube cookie is drawn one face at a time, each texel reading the cube along the
// ray of the face camera through it (invViewProj), x flipped as every cube read in this
// engine is. The quad's uv has v = 0 at the TOP, so NDC y = 1 - 2v. Level 0 explicitly.
//
// @variant 2d: COOKIE_2D=1
// @variant cube: COOKIE_CUBE=1
import vtquad;
#include "bindings.slang"

struct CookieUniforms
{
    float4x4 invViewProj;
};
VT_DECLARE_QUAD_UNIFORMS(CookieUniforms, u)
#if defined(COOKIE_CUBE)
VT_DECLARE_QUAD_CUBE_INPUT0(blitTexture)
#else
VT_DECLARE_QUAD_INPUT0(blitTexture)
#endif

[shader("vertex")]
QuadVarying cookieBlitVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 cookieBlitFragment(QuadVarying input) : SV_Target0
{
#if defined(COOKIE_CUBE)
    const float4 projPos = float4(input.uv.x * 2.0 - 1.0, 1.0 - input.uv.y * 2.0, 0.5, 1.0);
    const float3 dir = mul(u.invViewProj, projPos).xyz;
    return blitTexture.SampleLevel(float3(-dir.x, dir.y, dir.z), 0.0);
#else
    return blitTexture.SampleLevel(input.uv, 0.0);
#endif
}
