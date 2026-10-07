// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The per-rect depth clear of the shadow atlas: a fullscreen triangle whose every fragment
// lands at depth 1.0, drawn under the depth test ALWAYS into the current viewport. No
// colour output on purpose: the pass has no colour attachment, and a fragment stage that
// declares one anyway is a MoltenVK hazard that silently drops the depth write.
import vtquad;

struct ClearDepthVarying
{
    float4 position : SV_Position;
};

[shader("vertex")]
ClearDepthVarying clearDepthVertex(QuadVertexIn input)
{
    ClearDepthVarying output;
    output.position = float4(input.position.xy, 1.0, 1.0);
    return output;
}

[shader("fragment")]
void clearDepthFragment(ClearDepthVarying input)
{
}
