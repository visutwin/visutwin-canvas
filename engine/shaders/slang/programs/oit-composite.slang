// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 08.11.2026
//
// Order-independent transparency's composite (RenderPassOit): the weighted average colour of
// the accumulation (its sum of colour * alpha * weight over its sum of alpha * weight), blended
// over the camera's target by the coverage the second attachment holds (1 - prod(1 - alpha)).
// The accumulation targets are the size of the camera's target, so each fragment reads the
// texel under it. A pixel nothing transparent covered has coverage 0 and leaves the target alone.
import vtquad;
#include "bindings.slang"

VT_DECLARE_QUAD_INPUT0(oitAccumulation)
VT_DECLARE_QUAD_INPUT1(oitCoverage)

[shader("vertex")]
QuadVarying oitCompositeVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 oitCompositeFragment(QuadVarying input) : SV_Target0
{
    const float coverage = saturate(oitCoverage.Sample(input.uv).r);
    const float4 accumulation = oitAccumulation.Sample(input.uv);
    const float3 average = accumulation.rgb / clamp(accumulation.a, 1e-4, 5e4);
    return float4(average, coverage);
}
