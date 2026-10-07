// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The outline renderer's last pass: the processed outline texture over the back buffer,
// with the pass's standard alpha blend.
import vtquad;
#include "bindings.slang"

VT_DECLARE_QUAD_INPUT0(source)

[shader("vertex")]
QuadVarying outlineBlendVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 outlineBlendFragment(QuadVarying input) : SV_Target0
{
    return source.Sample(input.uv);
}
