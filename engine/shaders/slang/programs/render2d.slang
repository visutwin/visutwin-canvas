// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The performance HUD's 2D renderer. Every quad carries a mode, constant across it, so
// each branch is uniform within a quad: a solid rectangle fetches nothing, text and graphs
// fetch once.
//
// Vertex stream: the engine's packed 56-byte layout, reinterpreted.
//   position = (x, y, mode), x and y in [0, 1] of the target from its bottom-left corner
//   normal   = colour rgb, display space
//   uv0      = texture coordinate (v = 0 the texture's top row)
//   tangent  = (1 / quad height in points, height fraction from the quad bottom, colour alpha, 0)
// Modes: 0 solid, 1 regular text on page 0, 2 graph, 3 bold text on page 0, 4 regular text
// on page 1, 5 bold text on page 1.
//
// Uniform block (the material slot): clr multiplies every fragment; params is (graph cursor
// u, MSDF pixel range, page 0 width, page 0 height); pages is (page 1 width, page 1 height).
// Textures, on the material slots that keep their own sampler: 0 and 1 the regular and
// bold page 0, 4 and 5 the regular and bold page 1, 3 the graph history (REPEAT in u, so
// the cursor scrolls it). Graph texel: r is the sample over the row's scale, a is nonzero
// where a sample exists and its value is the height of the budget line.
#include "bindings.slang"

struct Render2dUniforms
{
    float4 clr;
    float4 params;
    float4 pages;
};
VT_DECLARE_MATERIAL_UNIFORMS(Render2dUniforms, u)
VT_DECLARE_baseColorMap
VT_DECLARE_normalMap
VT_DECLARE_metalRoughMap
VT_DECLARE_occlusionMap
VT_DECLARE_emissiveMap
#define regularPage0 baseColorMap
#define boldPage0 normalMap
#define graphTexture metalRoughMap
#define regularPage1 occlusionMap
#define boldPage1 emissiveMap

struct VertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
    [[vk::location(3)]] float4 tangent : TANGENT;
};

struct Varyings
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float2 graph : TEXCOORD1;   // 1 / quad height, height fraction
    float mode : TEXCOORD2;
};

[shader("vertex")]
Varyings render2dVertex(VertexIn input)
{
    Varyings output;
    output.position = float4(input.position.xy * 2.0 - 1.0, 0.5, 1.0);
    output.color = float4(input.normal, input.tangent.z);
    output.uv = input.uv0;
    output.graph = input.tangent.xy;
    output.mode = input.position.z;
    return output;
}

// Coverage from a distance-field sample; the edge is one screen pixel wide whatever the
// glyph's magnification.
float render2dMsdf(float3 s, float2 uvWidth, float pixelRange, float2 pageSize)
{
    const float distance = max(min(s.r, s.g), min(max(s.r, s.g), s.b));
    const float2 unitRange = float2(pixelRange, pixelRange) / max(pageSize, float2(1.0, 1.0));
    const float screenPxRange = max(0.5 * dot(unitRange, 1.0 / max(uvWidth, float2(1e-6, 1e-6))), 1.0);
    return saturate(screenPxRange * (distance - 0.5) + 0.5);
}

[shader("fragment")]
float4 render2dFragment(Varyings input) : SV_Target0
{
    // Derivatives before any branch: a 2x2 pixel quad can straddle two HUD quads of
    // different modes, and a derivative taken inside the branch is then undefined.
    const float2 uvWidth = fwidth(input.uv);
    float alpha = 1.0;
    if (input.mode > 4.5) {
        alpha = render2dMsdf(boldPage1.SampleLevel(input.uv, 0.0).rgb, uvWidth, u.params.y, u.pages.xy);
    } else if (input.mode > 3.5) {
        alpha = render2dMsdf(regularPage1.SampleLevel(input.uv, 0.0).rgb, uvWidth, u.params.y, u.pages.xy);
    } else if (input.mode > 2.5) {
        alpha = render2dMsdf(boldPage0.SampleLevel(input.uv, 0.0).rgb, uvWidth, u.params.y, u.params.zw);
    } else if (input.mode > 1.5) {
        const float4 s = graphTexture.SampleLevel(float2(input.uv.x + u.params.x, input.uv.y), 0.0);
        const float fill = step(input.graph.y, s.r) * 0.18;
        const float edge = (1.0 - step(input.graph.x, abs(input.graph.y - s.r))) * 0.60;
        const float budget = (1.0 - step(input.graph.x * 0.5, abs(input.graph.y - s.a))) * 0.16;
        alpha = max(fill, max(edge, budget)) * step(0.001, s.a);
    } else if (input.mode > 0.5) {
        alpha = render2dMsdf(regularPage0.SampleLevel(input.uv, 0.0).rgb, uvWidth, u.params.y, u.params.zw);
    }
    return float4(input.color.rgb, input.color.a * alpha) * u.clr;
}
