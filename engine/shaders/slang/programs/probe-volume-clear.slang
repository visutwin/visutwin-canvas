// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
// Clears the tiles of a probe volume bake's atlas (ProbeVolumeBaker) before its cameras render
// into them: the cameras render into one atlas each frame, a few probes at a time, and a pass's
// own clear would clear the whole atlas. A fullscreen triangle drawn into the tile's viewport,
// every fragment at depth 1 under the depth test ALWAYS, every colour attachment set to the
// colour of the uniform block. One variant per attachment count.
// @variant mrt1: VT_CLEAR_OUTPUTS=1
// @variant mrt2: VT_CLEAR_OUTPUTS=2
// @variant mrt3: VT_CLEAR_OUTPUTS=3
import vtquad;
#include "bindings.slang"

struct ProbeClearUniforms
{
    float4 color;
};
VT_DECLARE_QUAD_UNIFORMS(ProbeClearUniforms, u)

struct ProbeClearVarying
{
    float4 position : SV_Position;
};

#if VT_CLEAR_OUTPUTS == 3
struct ProbeClearOut
{
    float4 color0 : SV_Target0;
    float4 color1 : SV_Target1;
    float4 color2 : SV_Target2;
};
#elif VT_CLEAR_OUTPUTS == 2
struct ProbeClearOut
{
    float4 color0 : SV_Target0;
    float4 color1 : SV_Target1;
};
#else
struct ProbeClearOut
{
    float4 color0 : SV_Target0;
};
#endif

[shader("vertex")]
ProbeClearVarying probeClearVertex(QuadVertexIn input)
{
    ProbeClearVarying output;
    output.position = float4(input.position.xy, 1.0, 1.0);
    return output;
}

[shader("fragment")]
ProbeClearOut probeClearFragment(ProbeClearVarying input)
{
    ProbeClearOut output;
    output.color0 = u.color;
#if VT_CLEAR_OUTPUTS >= 2
    output.color1 = u.color;
#endif
#if VT_CLEAR_OUTPUTS == 3
    output.color2 = u.color;
#endif
    return output;
}
