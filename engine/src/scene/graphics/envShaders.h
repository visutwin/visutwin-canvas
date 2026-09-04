// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Shaders for the environment bakes, authored once per language and selected by
// GraphicsDevice::shaderLanguage(). These run over QuadRender inside a
// beginOfflineWork scope, so there is no pass class per backend — the same
// arrangement the fullscreen effects already use.
//
// Layout contract, shared with every other quad effect: the source texture is on
// fragment slot 0 (MSL texture(0) / GLSL set 1 binding 0) and the uniform block
// rides the per-draw material slot (MSL buffer(3) / GLSL set 0 binding 0).
//
#pragma once

#include <cstdint>

namespace visutwin::canvas::env_shaders
{
    /// Uniforms for the equirect-to-cubemap face pass. Scalars only, so MSL and
    /// std140 pack this identically and both shaders can declare one field list.
    struct alignas(16) EquirectToCubeUniforms
    {
        uint32_t face = 0u;        // 0..5, the cube face being rendered
        uint32_t decodeSrgb = 0u;  // decode the source from gamma before writing
        uint32_t _pad0 = 0u;
        uint32_t _pad1 = 0u;
    };
    static_assert(sizeof(EquirectToCubeUniforms) == 16);

    // The face mapping matches the CPU faceUvToDir helper this replaced, so the
    // cubemap layout stays compatible with the convolve pass's X flip.
    constexpr const char* EQUIRECT_TO_CUBE_MSL = R"(
#include <metal_stdlib>
using namespace metal;

constant float PI = 3.141592653589793;

struct QuadVertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv0      [[attribute(2)]];
    float4 tangent  [[attribute(3)]];
    float2 uv1      [[attribute(4)]];
};

struct FaceVarying {
    float4 position [[position]];
    float2 uv;
};

struct FaceUniforms {
    uint face;
    uint decodeSrgb;
    uint pad0;
    uint pad1;
};

vertex FaceVarying equirectToCubeVertex(QuadVertexIn in [[stage_in]])
{
    FaceVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

static float3 faceUvToDir(uint face, float2 uv)
{
    const float sc = uv.x * 2.0 - 1.0;
    const float tc = uv.y * 2.0 - 1.0;
    float3 dir;
    if      (face == 0u) dir = float3( 1.0, -tc, -sc);
    else if (face == 1u) dir = float3(-1.0, -tc,  sc);
    else if (face == 2u) dir = float3( sc,   1.0, tc);
    else if (face == 3u) dir = float3( sc,  -1.0,-tc);
    else if (face == 4u) dir = float3( sc,  -tc,  1.0);
    else                 dir = float3(-sc,  -tc, -1.0);
    return normalize(dir);
}

static float2 dirToUvEquirect(float3 dir)
{
    const float phi   = atan2(dir.x, dir.z);
    const float theta = asin(clamp(dir.y, -1.0, 1.0));
    return float2(phi / (2.0 * PI) + 0.5, 1.0 - (theta / PI + 0.5));
}

fragment float4 equirectToCubeFragment(
    FaceVarying in [[stage_in]],
    texture2d<float> sourceEquirect [[texture(0)]],
    sampler          linearSampler  [[sampler(0)]],
    constant FaceUniforms& u        [[buffer(3)]])
{
    const float2 uv = clamp(in.uv, float2(0.0), float2(1.0));
    const float3 dir = faceUvToDir(u.face, uv);
    const float4 raw = sourceEquirect.sample(linearSampler, dirToUvEquirect(dir));
    const float3 color = (u.decodeSrgb != 0u)
        ? pow(max(raw.rgb, float3(0.0)), float3(2.2)) : raw.rgb;
    return float4(color, 1.0);
}
)";

    constexpr const char* EQUIRECT_TO_CUBE_GLSL = R"(
#version 450

const float PI = 3.141592653589793;

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
layout(set = 0, binding = 0) uniform FaceUniforms {
    uint face;
    uint decodeSrgb;
    uint pad0;
    uint pad1;
} u;
layout(set = 1, binding = 0) uniform sampler2D sourceEquirect;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

vec3 faceUvToDir(uint face, vec2 uv) {
    float sc = uv.x * 2.0 - 1.0;
    float tc = uv.y * 2.0 - 1.0;
    vec3 dir;
    if      (face == 0u) dir = vec3( 1.0, -tc, -sc);
    else if (face == 1u) dir = vec3(-1.0, -tc,  sc);
    else if (face == 2u) dir = vec3( sc,   1.0, tc);
    else if (face == 3u) dir = vec3( sc,  -1.0,-tc);
    else if (face == 4u) dir = vec3( sc,  -tc,  1.0);
    else                 dir = vec3(-sc,  -tc, -1.0);
    return normalize(dir);
}

vec2 dirToUvEquirect(vec3 dir) {
    float phi   = atan(dir.x, dir.z);
    float theta = asin(clamp(dir.y, -1.0, 1.0));
    return vec2(phi / (2.0 * PI) + 0.5, 1.0 - (theta / PI + 0.5));
}

void main() {
    vec2 uv = clamp(vUv, vec2(0.0), vec2(1.0));
    vec3 dir = faceUvToDir(u.face, uv);
    vec4 raw = texture(sourceEquirect, dirToUvEquirect(dir));
    vec3 color = (u.decodeSrgb != 0u)
        ? pow(max(raw.rgb, vec3(0.0)), vec3(2.2)) : raw.rgb;
    fragColor = vec4(color, 1.0);
}
#endif
)";

    /// Uniforms for the reprojection pass. `uvMod` applies the seam expansion
    /// (upstream bakes a border so bilinear taps at a rect edge stay inside it);
    /// the projection ids mirror TextureProjection in platform/graphics/constants.h.
    struct alignas(16) ReprojectUniforms
    {
        float uvMod[4] = {1.0f, 1.0f, 0.0f, 0.0f};
        uint32_t sourceProjection = 2u;
        uint32_t encodeRgbp = 0u;
        uint32_t decodeSrgb = 0u;
        uint32_t targetProjection = 2u;
    };
    static_assert(sizeof(ReprojectUniforms) == 32);

    // The direction maths is shared by both source variants; only the sampling
    // line differs, so each language carries one body and a SRC_CUBE switch. A
    // cube source needs its own variant because a descriptor holding a cube view
    // cannot serve a shader that declares sampler2D.
    constexpr const char* REPROJECT_MSL = R"(
#include <metal_stdlib>
using namespace metal;

constant float PI = 3.141592653589793;
constant uint PROJ_OCTAHEDRAL = 3u;

struct QuadVertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv0      [[attribute(2)]];
    float4 tangent  [[attribute(3)]];
    float2 uv1      [[attribute(4)]];
};

struct ReprojectVarying {
    float4 position [[position]];
    float2 uv;
};

struct ReprojectUniforms {
    float4 uvMod;
    uint sourceProjection;
    uint encodeRgbp;
    uint decodeSrgb;
    uint targetProjection;
};

vertex ReprojectVarying reprojectVertex(QuadVertexIn in [[stage_in]])
{
    ReprojectVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

static float3 uvToDirEquirect(float2 uv)
{
    const float phi   = (uv.x * 2.0 - 1.0) * PI;
    const float theta = (1.0 - uv.y) * PI - PI * 0.5;
    const float c = cos(theta);
    return float3(sin(phi) * c, sin(theta), cos(phi) * c);
}

static float2 dirToUvEquirect(float3 dir)
{
    const float phi   = atan2(dir.x, dir.z);
    const float theta = asin(clamp(dir.y, -1.0, 1.0));
    return float2(phi / (2.0 * PI) + 0.5, 1.0 - (theta / PI + 0.5));
}

static float3 uvToDirOctahedral(float2 uv)
{
    const float2 f = uv * 2.0 - 1.0;
    float3 n = float3(f.x, 1.0 - abs(f.x) - abs(f.y), f.y);
    const float t = max(-n.y, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.z += (n.z >= 0.0) ? -t : t;
    return normalize(n);
}

static float2 dirToUvOctahedral(float3 dir)
{
    const float3 a = abs(dir);
    const float invL1 = 1.0 / max(a.x + a.y + a.z, 1e-8);
    float2 t = float2(dir.x, dir.z) * invL1;
    if (dir.y < 0.0) {
        t = (1.0 - abs(float2(t.y, t.x))) *
            float2(t.x >= 0.0 ? 1.0 : -1.0, t.y >= 0.0 ? 1.0 : -1.0);
    }
    return t * 0.5 + 0.5;
}

static float4 packRgbp(float3 color)
{
    const float3 lin = max(color, float3(0.0));
    const float3 s = float3(sqrt(lin.r), sqrt(lin.g), sqrt(lin.b));
    const float maxVal = max(max(s.r, s.g), max(s.b, 1.0 / 255.0));
    const float a = clamp((8.0 - maxVal) / 7.0, 0.0, 1.0);
    const float scale = -a * 7.0 + 8.0;
    return float4(clamp(s / scale, 0.0, 1.0), a);
}

fragment float4 reprojectFragment(
    ReprojectVarying in [[stage_in]],
#ifdef SRC_CUBE
    texturecube<float> sourceTexture [[texture(0)]],
#else
    texture2d<float>   sourceTexture [[texture(0)]],
#endif
    sampler            linearSampler [[sampler(0)]],
    constant ReprojectUniforms& u    [[buffer(3)]])
{
    const float2 uv = in.uv * u.uvMod.xy + u.uvMod.zw;
    const float3 dir = normalize((u.targetProjection == PROJ_OCTAHEDRAL)
        ? uvToDirOctahedral(uv) : uvToDirEquirect(uv));

#ifdef SRC_CUBE
    // The engine's atlas lookup handedness: negate X, as the sky paths do.
    const float4 raw = sourceTexture.sample(linearSampler, float3(-dir.x, dir.y, dir.z));
#else
    const float2 srcUv = (u.sourceProjection == PROJ_OCTAHEDRAL)
        ? dirToUvOctahedral(dir) : dirToUvEquirect(dir);
    const float4 raw = sourceTexture.sample(linearSampler, srcUv);
#endif
    const float3 color = (u.decodeSrgb != 0u)
        ? pow(max(raw.rgb, float3(0.0)), float3(2.2)) : raw.rgb;

    return (u.encodeRgbp != 0u) ? packRgbp(color) : float4(color, 1.0);
}
)";

    constexpr const char* REPROJECT_GLSL = R"(
const float PI = 3.141592653589793;
const uint PROJ_OCTAHEDRAL = 3u;

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
layout(set = 0, binding = 0) uniform ReprojectUniforms {
    vec4 uvMod;
    uint sourceProjection;
    uint encodeRgbp;
    uint decodeSrgb;
    uint targetProjection;
} u;
#ifdef SRC_CUBE
layout(set = 1, binding = 0) uniform samplerCube sourceTexture;
#else
layout(set = 1, binding = 0) uniform sampler2D sourceTexture;
#endif
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

vec3 uvToDirEquirect(vec2 uv) {
    float phi   = (uv.x * 2.0 - 1.0) * PI;
    float theta = (1.0 - uv.y) * PI - PI * 0.5;
    float c = cos(theta);
    return vec3(sin(phi) * c, sin(theta), cos(phi) * c);
}

vec2 dirToUvEquirect(vec3 dir) {
    float phi   = atan(dir.x, dir.z);
    float theta = asin(clamp(dir.y, -1.0, 1.0));
    return vec2(phi / (2.0 * PI) + 0.5, 1.0 - (theta / PI + 0.5));
}

vec3 uvToDirOctahedral(vec2 uv) {
    vec2 f = uv * 2.0 - 1.0;
    vec3 n = vec3(f.x, 1.0 - abs(f.x) - abs(f.y), f.y);
    float t = max(-n.y, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.z += (n.z >= 0.0) ? -t : t;
    return normalize(n);
}

vec2 dirToUvOctahedral(vec3 dir) {
    vec3 a = abs(dir);
    float invL1 = 1.0 / max(a.x + a.y + a.z, 1e-8);
    vec2 t = vec2(dir.x, dir.z) * invL1;
    if (dir.y < 0.0) {
        t = (1.0 - abs(vec2(t.y, t.x))) *
            vec2(t.x >= 0.0 ? 1.0 : -1.0, t.y >= 0.0 ? 1.0 : -1.0);
    }
    return t * 0.5 + 0.5;
}

vec4 packRgbp(vec3 color) {
    vec3 lin = max(color, vec3(0.0));
    vec3 s = sqrt(lin);
    float maxVal = max(max(s.r, s.g), max(s.b, 1.0 / 255.0));
    float a = clamp((8.0 - maxVal) / 7.0, 0.0, 1.0);
    float scale = -a * 7.0 + 8.0;
    return vec4(clamp(s / scale, 0.0, 1.0), a);
}

void main() {
    vec2 uv = vUv * u.uvMod.xy + u.uvMod.zw;
    vec3 dir = normalize((u.targetProjection == PROJ_OCTAHEDRAL)
        ? uvToDirOctahedral(uv) : uvToDirEquirect(uv));

#ifdef SRC_CUBE
    vec4 raw = texture(sourceTexture, vec3(-dir.x, dir.y, dir.z));
#else
    vec2 srcUv = (u.sourceProjection == PROJ_OCTAHEDRAL)
        ? dirToUvOctahedral(dir) : dirToUvEquirect(dir);
    vec4 raw = texture(sourceTexture, srcUv);
#endif
    vec3 color = (u.decodeSrgb != 0u)
        ? pow(max(raw.rgb, vec3(0.0)), vec3(2.2)) : raw.rgb;

    fragColor = (u.encodeRgbp != 0u) ? packRgbp(color) : vec4(color, 1.0);
}
#endif
)";
}
