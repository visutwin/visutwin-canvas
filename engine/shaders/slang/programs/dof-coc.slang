// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The depth-of-field circle of confusion from the scene depth: a dead zone of +/- half the
// focus range around the focus distance, then a ramp over the FULL focus range (the
// single-pass depth of field in compose uses the same). r = far CoC, g = near CoC when the
// near blur is on. Depth is point sampled.
import vtquad;
#include "bindings.slang"

struct CoCUniforms
{
    float4 focus;   // x = focusDistance, y = focusRange, z = cameraNear, w = cameraFar
    float4 flags;   // x = nearBlur
};
VT_DECLARE_QUAD_UNIFORMS(CoCUniforms, u)
VT_DECLARE_QUAD_DEPTH_INPUT0(depthTexture)

float getLinearDepth(float rawDepth, float cameraNear, float cameraFar)
{
    return (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
}

[shader("vertex")]
QuadVarying cocVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 cocFragment(QuadVarying input) : SV_Target0
{
    const float2 uv = clamp(input.uv, float2(0.0, 0.0), float2(1.0, 1.0));
    const float rawDepth = depthTexture.Sample(uv);
    const float linearDepth = getLinearDepth(rawDepth, u.focus.z, u.focus.w);

    const float invRange = 1.0 / max(u.focus.y, 0.001);
    const float farRange = u.focus.x + u.focus.y * 0.5;
    const float cocFar = saturate((linearDepth - farRange) * invRange);

    float cocNear = 0.0;
    if (u.flags.x > 0.5) {
        const float nearRange = u.focus.x - u.focus.y * 0.5;
        cocNear = saturate((nearRange - linearDepth) * invRange);
    }
    return float4(cocFar, cocNear, 0.0, 1.0);
}
