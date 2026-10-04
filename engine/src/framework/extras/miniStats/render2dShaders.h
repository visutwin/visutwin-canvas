// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.10.2026
//
// The Render2d shader, in MSL and GLSL. Every quad carries a mode, constant across it,
// so each branch is uniform within a quad: a solid rectangle fetches nothing, text and
// graphs fetch once.
//
// Vertex stream: the engine's packed 56-byte layout, reinterpreted.
//   position = (x, y, mode), x and y in [0, 1] of the target from its bottom-left corner
//   normal   = colour rgb, display space
//   uv0      = texture coordinate (v = 0 the texture's top row)
//   tangent  = (1 / quad height in points, height fraction from the quad bottom, colour alpha, 0)
//
// Uniform block (the material slot): clr multiplies every fragment; params is
// (graph cursor u, MSDF pixel range, MSDF atlas width, MSDF atlas height).
//
// Textures (material slots, the same numbers on both backends): 0 the regular font page,
// 1 the bold font page, 3 the graph history (REPEAT in u, so the cursor scrolls it).
//
// Graph texel: r is the sample over the row's scale, a is nonzero where a sample exists
// and its value is the height of the budget line.
#pragma once

namespace visutwin::canvas::render2d
{
    inline constexpr const char* kVertexEntry = "render2dVertex";
    inline constexpr const char* kFragmentEntry = "render2dFragment";

    inline constexpr const char* kMsl = R"MSL(
using namespace metal;

struct VertexData {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv0      [[attribute(2)]];
    float4 tangent  [[attribute(3)]];
    float2 uv1      [[attribute(4)]];
};

struct Render2dUniforms {
    float4 clr;
    float4 params;
};

struct Varyings {
    float4 position [[position]];
    float4 color;
    float2 uv;
    float2 graph;   // 1 / quad height, height fraction
    float mode;
};

vertex Varyings render2dVertex(VertexData v [[stage_in]])
{
    Varyings out;
    out.position = float4(v.position.xy * 2.0 - 1.0, 0.5, 1.0);
    out.color = float4(v.normal, v.tangent.z);
    out.uv = v.uv0;
    out.graph = v.tangent.xy;
    out.mode = v.position.z;
    return out;
}

static float render2dMsdf(texture2d<float> page, sampler pageSampler, float2 uv, float2 uvWidth, float4 params)
{
    const float3 s = page.sample(pageSampler, uv, level(0.0)).rgb;
    const float distance = max(min(s.r, s.g), min(max(s.r, s.g), s.b));
    const float2 unitRange = float2(params.y) / max(params.zw, float2(1.0));
    const float screenPxRange = max(0.5 * dot(unitRange, 1.0 / max(uvWidth, float2(1e-6))), 1.0);
    return saturate(screenPxRange * (distance - 0.5) + 0.5);
}

fragment float4 render2dFragment(Varyings in [[stage_in]],
                                 constant Render2dUniforms& u [[buffer(3)]],
                                 texture2d<float> regularPage [[texture(0)]],
                                 sampler regularSampler [[sampler(1)]],
                                 texture2d<float> boldPage [[texture(1)]],
                                 sampler boldSampler [[sampler(2)]],
                                 texture2d<float> graphTexture [[texture(3)]],
                                 sampler graphSampler [[sampler(3)]])
{
    // Derivatives before any branch: a 2x2 pixel quad can straddle two HUD quads of
    // different modes, and a derivative taken inside the branch is then undefined.
    const float2 uvWidth = fwidth(in.uv);
    float alpha = 1.0;
    if (in.mode > 2.5) {
        alpha = render2dMsdf(boldPage, boldSampler, in.uv, uvWidth, u.params);
    } else if (in.mode > 1.5) {
        const float4 s = graphTexture.sample(graphSampler, float2(in.uv.x + u.params.x, in.uv.y), level(0.0));
        const float fill = step(in.graph.y, s.r) * 0.18;
        const float edge = (1.0 - step(in.graph.x, abs(in.graph.y - s.r))) * 0.60;
        const float budget = (1.0 - step(in.graph.x * 0.5, abs(in.graph.y - s.a))) * 0.16;
        alpha = max(fill, max(edge, budget)) * step(0.001, s.a);
    } else if (in.mode > 0.5) {
        alpha = render2dMsdf(regularPage, regularSampler, in.uv, uvWidth, u.params);
    }
    return float4(in.color.rgb, in.color.a * alpha) * u.clr;
}
)MSL";

    inline constexpr const char* kGlsl = R"GLSL(
#version 450

layout(set = 0, binding = 0) uniform Render2dUniforms {
    vec4 clr;
    vec4 params;
} u;

#ifdef VT_VERTEX_SHADER
layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec3 vertexNormal;
layout(location = 2) in vec2 vertexTexCoord0;
layout(location = 3) in vec4 vertexTangent;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUv;
layout(location = 2) out vec2 vGraph;
layout(location = 3) out float vMode;

void main() {
    gl_Position = vec4(vertexPosition.xy * 2.0 - 1.0, 0.5, 1.0);
    vColor = vec4(vertexNormal, vertexTangent.z);
    vUv = vertexTexCoord0;
    vGraph = vertexTangent.xy;
    vMode = vertexPosition.z;
}
#endif

#ifdef VT_FRAGMENT_SHADER
layout(set = 1, binding = 0) uniform sampler2D regularPage;
layout(set = 1, binding = 1) uniform sampler2D boldPage;
layout(set = 1, binding = 3) uniform sampler2D graphTexture;

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUv;
layout(location = 2) in vec2 vGraph;
layout(location = 3) in float vMode;

layout(location = 0) out vec4 fragColor;

float render2dMsdf(vec3 s, vec2 uvWidth) {
    float distance = max(min(s.r, s.g), min(max(s.r, s.g), s.b));
    vec2 unitRange = vec2(u.params.y) / max(u.params.zw, vec2(1.0));
    float screenPxRange = max(0.5 * dot(unitRange, 1.0 / max(uvWidth, vec2(1e-6))), 1.0);
    return clamp(screenPxRange * (distance - 0.5) + 0.5, 0.0, 1.0);
}

void main() {
    vec2 uvWidth = fwidth(vUv);
    float alpha = 1.0;
    if (vMode > 2.5) {
        alpha = render2dMsdf(textureLod(boldPage, vUv, 0.0).rgb, uvWidth);
    } else if (vMode > 1.5) {
        vec4 s = textureLod(graphTexture, vec2(vUv.x + u.params.x, vUv.y), 0.0);
        float fill = step(vGraph.y, s.r) * 0.18;
        float edge = (1.0 - step(vGraph.x, abs(vGraph.y - s.r))) * 0.60;
        float budget = (1.0 - step(vGraph.x * 0.5, abs(vGraph.y - s.a))) * 0.16;
        alpha = max(fill, max(edge, budget)) * step(0.001, s.a);
    } else if (vMode > 0.5) {
        alpha = render2dMsdf(textureLod(regularPage, vUv, 0.0).rgb, uvWidth);
    }
    fragColor = vec4(vColor.rgb, vColor.a * alpha) * u.clr;
}
#endif
)GLSL";
}
