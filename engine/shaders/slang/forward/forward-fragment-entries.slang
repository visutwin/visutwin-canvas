// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// The forward program's fragment entries, one per set of outputs; ProgramLibrary picks one per
// variant. Each shades the fragment (forwardShade) and hands it to forwardFragmentOutput.

ForwardFragmentOutputs forwardOutputs(ForwardVarying input, bool isFrontFace, out float4 color)
{
    color = forwardShade(input, isFrontFace);
    ForwardFragmentOutputs outputs;
    forwardFragmentOutput(color, outputs);
    return outputs;
}

[shader("fragment")]
float4 forwardFragment(ForwardVarying input, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    float4 color = forwardShade(input, isFrontFace);
    ForwardFragmentOutputs outputs = {};
    forwardFragmentOutput(color, outputs);
    return color;
}

// The first colour and the second source of a dual-source blend: both go to attachment 0, the
// second at index 1. The member's name is what puts it there on Metal: Slang emits it as
// [[color(1)]], which the bundle and the run-time compile rewrite to [[color(0), index(1)]]
// (tools/generate_slang_bundle.py, SlangCompiler).
struct ForwardDualSourceOut
{
    [[vk::location(0)]] float4 color : SV_Target0;
    [[vk::location(0), vk::index(1)]] float4 dualSourceSecondary : SV_Target1;
};

[shader("fragment")]
ForwardDualSourceOut forwardFragmentDualSource(ForwardVarying input, bool isFrontFace : SV_IsFrontFace)
{
    ForwardDualSourceOut result;
    result.dualSourceSecondary = forwardOutputs(input, isFrontFace, result.color).secondary;
    return result;
}

struct ForwardMrt2Out
{
    float4 color0 : SV_Target0;
    float4 color1 : SV_Target1;
};

[shader("fragment")]
ForwardMrt2Out forwardFragmentMrt2(ForwardVarying input, bool isFrontFace : SV_IsFrontFace)
{
    ForwardMrt2Out result;
    const ForwardFragmentOutputs outputs = forwardOutputs(input, isFrontFace, result.color0);
    result.color1 = outputs.color1;
    return result;
}

struct ForwardMrt3Out
{
    float4 color0 : SV_Target0;
    float4 color1 : SV_Target1;
    float4 color2 : SV_Target2;
};

[shader("fragment")]
ForwardMrt3Out forwardFragmentMrt3(ForwardVarying input, bool isFrontFace : SV_IsFrontFace)
{
    ForwardMrt3Out result;
    const ForwardFragmentOutputs outputs = forwardOutputs(input, isFrontFace, result.color0);
    result.color1 = outputs.color1;
    result.color2 = outputs.color2;
    return result;
}

struct ForwardMrt4Out
{
    float4 color0 : SV_Target0;
    float4 color1 : SV_Target1;
    float4 color2 : SV_Target2;
    float4 color3 : SV_Target3;
};

[shader("fragment")]
ForwardMrt4Out forwardFragmentMrt4(ForwardVarying input, bool isFrontFace : SV_IsFrontFace)
{
    ForwardMrt4Out result;
    const ForwardFragmentOutputs outputs = forwardOutputs(input, isFrontFace, result.color0);
    result.color1 = outputs.color1;
    result.color2 = outputs.color2;
    result.color3 = outputs.color3;
    return result;
}
