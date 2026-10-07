// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// What every fullscreen pass and bake shares: the vertex input (the packed mesh layout
// QuadRender draws its oversized triangle with: position at attribute / location 0, uv0
// at 2), the varying and the pass-through vertex function. A program imports this and
// declares its own vertex entry point over quadPassThrough, so the entry points stay in
// the program module.
//
// Named vtquad, not quad: the bare name collided with something of Slang's own.
module vtquad;

public struct QuadVertexIn
{
    public float3 position : POSITION;
    public float3 normal : NORMAL;
    public float2 uv0 : TEXCOORD0;
};

public struct QuadVarying
{
    public float4 position : SV_Position;
    public float2 uv : TEXCOORD0;
};

public QuadVarying quadPassThrough(QuadVertexIn input)
{
    QuadVarying output;
    output.position = float4(input.position, 1.0);
    output.uv = input.uv0;
    return output;
}
