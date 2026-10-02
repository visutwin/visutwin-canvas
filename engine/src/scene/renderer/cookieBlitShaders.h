// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Upstream's cookie blit shaders (cookie-blit.js, cookie-blit-2d.js, cookie-blit-cube.js):
// copy a light cookie into its rect of the clustered cookie atlas. A 2D cookie is copied
// as is; a cube cookie is drawn one face at a time, each texel reading the cube along the
// ray of the face camera through it (`invViewProj`), x flipped as every cube read in this
// engine is (the cube convention of the skybox, probes and non-clustered cookies).
//
// Layout, shared with every quad effect: the source is fragment slot 0 (MSL texture(0) /
// GLSL set 1 binding 0) and the uniforms ride the material slot (MSL buffer(3) / GLSL set
// 0 binding 0). The quad's uv has v = 0 at the TOP, so NDC y = 1 - 2v.
//
#pragma once

#include <cstdint>

namespace visutwin::canvas::cookie_shaders
{
    struct alignas(16) CookieBlitUniforms
    {
        float invViewProj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    };
    static_assert(sizeof(CookieBlitUniforms) == 64);

    constexpr const char* COOKIE_BLIT_MSL = R"(
#include <metal_stdlib>
using namespace metal;

struct QuadVertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv0      [[attribute(2)]];
    float4 tangent  [[attribute(3)]];
    float2 uv1      [[attribute(4)]];
};

struct CookieVarying {
    float4 position [[position]];
    float2 uv;
};

struct CookieUniforms {
    float4x4 invViewProj;
};

vertex CookieVarying cookieBlitVertex(QuadVertexIn in [[stage_in]])
{
    CookieVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

constexpr sampler cookieBlitSampler(coord::normalized, filter::linear, address::clamp_to_edge);

fragment float4 cookieBlitFragment(
    CookieVarying in [[stage_in]],
    constant CookieUniforms& u [[buffer(3)]],
#ifdef SRC_CUBE
    texturecube<float> blitTexture [[texture(0)]])
{
    const float4 projPos = float4(in.uv.x * 2.0 - 1.0, 1.0 - in.uv.y * 2.0, 0.5, 1.0);
    const float3 dir = (u.invViewProj * projPos).xyz;
    return blitTexture.sample(cookieBlitSampler, float3(-dir.x, dir.y, dir.z), level(0));
}
#else
    texture2d<float> blitTexture [[texture(0)]])
{
    return blitTexture.sample(cookieBlitSampler, in.uv, level(0));
}
#endif
)";

    constexpr const char* COOKIE_BLIT_GLSL = R"(
#ifdef VT_VERTEX_SHADER
layout(location = 0) in vec3 vertexPosition;
layout(location = 2) in vec2 vertexUv0;
layout(location = 0) out vec2 vUv;
void main() {
    vUv = vertexUv0;
    gl_Position = vec4(vertexPosition, 1.0);
}
#endif

#ifdef VT_FRAGMENT_SHADER
layout(set = 0, binding = 0) uniform CookieUniforms {
    mat4 invViewProj;
} u;
#ifdef SRC_CUBE
layout(set = 1, binding = 0) uniform samplerCube blitTexture;
#else
layout(set = 1, binding = 0) uniform sampler2D blitTexture;
#endif
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

void main() {
#ifdef SRC_CUBE
    vec4 projPos = vec4(vUv.x * 2.0 - 1.0, 1.0 - vUv.y * 2.0, 0.5, 1.0);
    vec3 dir = (u.invViewProj * projPos).xyz;
    fragColor = textureLod(blitTexture, vec3(-dir.x, dir.y, dir.z), 0.0);
#else
    fragColor = textureLod(blitTexture, vUv, 0.0);
#endif
}
#endif
)";
}
