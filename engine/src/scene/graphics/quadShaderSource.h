// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The source every fullscreen pass and bake shares: the vertex input (the packed mesh
// layout QuadRender draws its triangle with), the varying, and the pass-through vertex
// stage, once per language. They are string-literal macros so a shader source stays one
// compile-time literal: write the prelude, then the vertex stage, then the body, as
// adjacent literals —
//
//     constexpr const char* MY_MSL = VT_QUAD_MSL_PRELUDE VT_QUAD_MSL_VERTEX(myVertex) R"(
//     fragment float4 myFragment(QuadVarying in [[stage_in]], ...) { ... }
//     )";
//
// and on GLSL put VT_QUAD_GLSL_VERTEX where the stage's #ifdef VT_VERTEX_SHADER block
// goes; the fragment stage reads `layout(location = 0) in vec2 vUv`.
//
#pragma once

// MSL: the standard library, the vertex input and the varying.
#define VT_QUAD_MSL_PRELUDE R"(
#include <metal_stdlib>
using namespace metal;

struct QuadVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv0 [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float2 uv1 [[attribute(4)]];
};

struct QuadVarying {
    float4 position [[position]];
    float2 uv;
};
)"

// MSL: the pass-through vertex stage, under the entry point name the pass registers.
#define VT_QUAD_MSL_VERTEX(NAME) R"(
vertex QuadVarying )" #NAME R"((QuadVertexIn in [[stage_in]])
{
    QuadVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}
)"

// GLSL: the pass-through vertex stage, compiled when VT_VERTEX_SHADER is defined.
#define VT_QUAD_GLSL_VERTEX R"(
#ifdef VT_VERTEX_SHADER
layout(location = 0) in vec3 vertexPosition;
layout(location = 2) in vec2 vertexUv0;
layout(location = 0) out vec2 vUv;
void main() {
    vUv = vertexUv0;
    gl_Position = vec4(vertexPosition, 1.0);
}
#endif
)"
