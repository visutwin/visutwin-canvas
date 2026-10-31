// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Resolves a multisampled depth texture into a single-sampled one (R32F) by the target's depth
// resolve mode: the first sample, or the nearest or farthest of them (RenderPassDepthResolve).
// The quad's uv has v = 0 at the TOP, as every texture's row 0.
import vtquad;
#include "bindings.slang"

struct DepthResolveUniforms
{
    uint4 params;   // x: the mode (0 sample 0, 1 min, 2 max), y, z: the texture's size, w: its samples
};
VT_DECLARE_QUAD_UNIFORMS(DepthResolveUniforms, u)
VT_DECLARE_QUAD_DEPTH_MS_INPUT0(msDepth)

[shader("vertex")]
QuadVarying depthResolveVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 depthResolveFragment(QuadVarying input) : SV_Target0
{
    const uint samples = u.params.w;
    const int2 coord = min(int2(input.uv * float2(u.params.yz)), int2(u.params.yz) - int2(1, 1));

    float depth = VT_LOAD_MS(msDepth, coord, 0);
    if (u.params.x == 1u) {
        for (uint s = 1u; s < samples; s++) {
            depth = min(depth, VT_LOAD_MS(msDepth, coord, int(s)));
        }
    } else if (u.params.x == 2u) {
        for (uint s = 1u; s < samples; s++) {
            depth = max(depth, VT_LOAD_MS(msDepth, coord, int(s)));
        }
    }
    return float4(depth, 0.0, 0.0, 1.0);
}
