// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// The reference grid of extras/grid/grid.h: anti-aliased lines of constant width on screen
// (the "pristine grid" construction), in levels of `subdivisions` cells that fade in and turn
// major as the camera zooms out, axis lines through the origin, a distance fade and a fade at
// glancing angles. One plane quad, moved in the vertex stage: an infinite grid follows the
// camera and spans the fade distance, a fixed one is sized by its node's scale.
//
// Feature word 0 (grid.cpp: GridSwitch): contrast (lines black over light parts of the scene
// and white over dark ones, from the scene colour grab), the depth pass (no colour, writes the
// depth of the lines where they cover more than half a pixel), and its dither.
//
// The camera is recovered from the view-projection, which is all a mesh draw hands its vertex
// stage: a perspective camera sits where clip x, y and w vanish; an orthographic one has no
// position, so the centre of its near plane stands in for it, and its far distance is
// measured from there.
import vtdraw;
#include "bindings.slang"

struct GridData
{
    float4 axisU;       // xyz the plane's u axis in the node's space, w: 1 for an infinite grid
    float4 axisV;       // xyz the plane's v axis, w: depth offset (a fraction of the eye distance)
    float4 params;      // fade start, fade end, spacing, subdivisions
    float4 thickness;   // major, minor and axis line widths in pixels, w: pixels per point
    float4 colorU;      // the line along u, gamma encoded
    float4 colorV;      // the line along v
    float4 majorColor;
    float4 minorColor;
    float4 output;      // x: 1 to draw the axes, y: 1 for a linear HDR target
};
VT_DECLARE_MATERIAL_UNIFORMS(GridData, grid)

VT_DECLARE_sceneColorGrab
// The forward pass's clamping sampler, at the Metal slot it binds it to.
[[vk::binding(12, 3)]] SamplerState linearClampSampler : register(s7);

static const uint kGridContrast = 1u << 0u;
static const uint kGridDepth = 1u << 1u;
static const uint kGridDepthDither = 1u << 2u;

// The fraction of the far clip the fade must end within.
static const float kFarFraction = 0.9;
// The cell size in points below which lines thin with their cells, so distant lines fade.
static const float kLineFadeCell = 64.0;
// The cell size in points where a level starts to fade in, and the levels drawn above it.
static const float kLevelFadeCell = 4.0;
static const int kLevelsAbove = 2;

bool gridSwitch(uint bit)
{
    return (vtFeatureMask0 & bit) != 0u;
}

struct VertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
};

struct Varyings
{
    float4 position : SV_Position;
    float2 gridPos;
    float3 worldPos;
    float4 clipPos;
    float3 toEye;
    nointerpolation float3 normal;
    nointerpolation float4 eye;     // xyz the camera position, w its far distance
};

// The point where three planes of the view-projection meet: rows a0, a1, a2 (their xyz) with
// a0.p = r.x, a1.p = r.y, a2.p = r.z.
float3 solvePlanes(float3 a0, float3 a1, float3 a2, float3 r)
{
    const float3 c12 = cross(a1, a2);
    return (r.x * c12 + r.y * cross(a2, a0) + r.z * cross(a0, a1)) / dot(a0, c12);
}

[shader("vertex")]
Varyings gridVertex(VertexIn input)
{
    // The rows of the view-projection: M[i] is its column i.
    const float4x4 vp = drawViewProjection();
    const float4 row0 = float4(vp[0].x, vp[1].x, vp[2].x, vp[3].x);
    const float4 row1 = float4(vp[0].y, vp[1].y, vp[2].y, vp[3].y);
    const float4 row2 = float4(vp[0].z, vp[1].z, vp[2].z, vp[3].z);
    const float4 row3 = float4(vp[0].w, vp[1].w, vp[2].w, vp[3].w);
    const bool ortho = dot(row3.xyz, row3.xyz) < 1e-12;

    float3 eye;
    float far;
    float3 back = float3(0.0);
    if (ortho) {
        // On the view axis at the near plane (GL clip z = -1), and at the far plane (z = 1).
        eye = solvePlanes(row0.xyz, row1.xyz, row2.xyz, float3(-row0.w, -row1.w, -1.0 - row2.w));
        const float3 farPoint = solvePlanes(row0.xyz, row1.xyz, row2.xyz, float3(-row0.w, -row1.w, 1.0 - row2.w));
        far = length(farPoint - eye);
        back = normalize(eye - farPoint);
    } else {
        eye = solvePlanes(row0.xyz, row1.xyz, row3.xyz, float3(-row0.w, -row1.w, -row3.w));
        // On the view axis where clip z = w.
        const float3 farPoint = solvePlanes(row0.xyz, row1.xyz, row2.xyz - row3.xyz,
            float3(-row0.w, -row1.w, row3.w - row2.w));
        far = length(farPoint - eye);
    }

    // The node's scale sizes a fixed grid, never its cells.
    const float4x4 model = drawModel();
    const float3 origin = model[3].xyz;
    const float3 axisU = mul(float4(grid.axisU.xyz, 0.0), model).xyz;
    const float3 axisV = mul(float4(grid.axisV.xyz, 0.0), model).xyz;
    const float3 u = normalize(axisU);
    const float3 v = normalize(axisV);
    const float infinite = grid.axisU.w;

    // An infinite grid follows the camera and spans the fade distance.
    const float reach = 2.0 * min(grid.params.y, far * kFarFraction);
    const float2 center = float2(dot(eye - origin, u), dot(eye - origin, v)) * infinite;
    const float2 extent = lerp(float2(length(axisU), length(axisV)), float2(reach), infinite);

    Varyings output;
    output.gridPos = center + input.position.xz * extent;
    output.worldPos = origin + u * output.gridPos.x + v * output.gridPos.y;

    // Offset along the view ray, so only the depth changes.
    float3 toEye = eye - output.worldPos;
    if (ortho) {
        toEye = back * dot(toEye, back);
    }
    float4 clip = mul(float4(output.worldPos + toEye * grid.axisV.w, 1.0), vp);
    output.clipPos = clip;
    output.toEye = toEye;
    output.normal = cross(u, v);
    output.eye = float4(eye, far);
    clip.z = 0.5 * (clip.z + clip.w);
    output.position = clip;
    return output;
}

// Lines `pixels` wide at a uv whose screen derivative is uvDeriv, faded per axis by `fade`.
float pristineGrid(float2 uv, float2 uvDeriv, float pixels, float2 fade)
{
    const float2 targetWidth = min(pixels * uvDeriv, float2(min(pixels / (kLineFadeCell * grid.thickness.w), 0.5)));
    const float2 drawWidth = clamp(targetWidth, uvDeriv, float2(0.5));
    const float2 lineAA = uvDeriv * 1.5;
    const float2 gridUV = 1.0 - abs(frac(uv) * 2.0 - 1.0);
    float2 grid2 = smoothstep(drawWidth + lineAA, drawWidth - lineAA, gridUV);
    grid2 *= saturate(targetWidth / drawWidth);
    grid2 = lerp(grid2, targetWidth, saturate(uvDeriv * 2.0 - 1.0));
    grid2 *= fade;
    return lerp(grid2.x, 1.0, grid2.y);
}

// Drops the origin line where the axes draw it.
float2 offAxis(float2 uv)
{
    return grid.output.x > 0.5 ? step(0.5, abs(uv)) : float2(1.0);
}

// Interleaved gradient noise, in [0, 1).
float gridNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

[shader("fragment")]
float4 gridFragment(Varyings input) : SV_Target0
{
    const float spacing = grid.params.z;
    const float subdivisions = grid.params.w;
    const float3 thickness = grid.thickness.xyz;
    const float showAxes = grid.output.x;

    const float2 pos = input.gridPos / spacing;
    const float2 dx = ddx(pos);
    const float2 dy = ddy(pos);
    const float2 uvDeriv = float2(length(float2(dx.x, dy.x)), length(float2(dx.y, dy.y)));
    const float epsilon = 1.0 / 255.0;

    // The level fading in, by cell size on screen; one fixed level without subdivisions.
    const float lod = subdivisions > 1.0 ?
        log(kLevelFadeCell * grid.thickness.w * max(uvDeriv.x, uvDeriv.y)) / log(subdivisions) :
        -1.0;
    const float finest = ceil(lod);

    // The coarsest level first; the finest fades in and the coarser turn major.
    float4 color = grid.minorColor;
    float line = 0.0;
    for (int i = kLevelsAbove; i >= 0; i--) {
        const float k = finest + float(i);
        const float scale = pow(subdivisions, -k);
        const float c = k - lod;
        const float major = saturate(c - 1.0);
        float l = pristineGrid(pos * scale, uvDeriv * scale, lerp(thickness.y, thickness.x, major), offAxis(pos * scale));
        l *= saturate(c);
        if (l > epsilon) {
            color = lerp(grid.minorColor, grid.majorColor, major);
            line = l;
            break;
        }
    }

    // The axis lines, from the coarsest level.
    const float topScale = pow(subdivisions, -(finest + float(kLevelsAbove)));
    const float2 top = pos * topScale;
    const float2 topDeriv = uvDeriv * topScale;
    const float lineU = abs(top.y) < 0.5 ? pristineGrid(top, topDeriv, thickness.z, float2(0.0, 1.0)) : 0.0;
    const float lineV = abs(top.x) < 0.5 ? pristineGrid(top, topDeriv, thickness.z, float2(1.0, 0.0)) : 0.0;
    const float axis = max(lineU, lineV) * showAxes;
    if (axis > epsilon && axis >= line) {
        color = lineU >= lineV ? grid.colorU : grid.colorV;
        line = axis;
    } else if (gridSwitch(kGridContrast)) {
        // Black over light, white over dark.
        const float2 ndc = input.clipPos.xy / input.clipPos.w;
        const float2 screenUV = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        const float3 under = sceneColorGrab.SampleLevel(linearClampSampler, screenUV, 0.0).rgb;
        color.rgb = float3(1.0 - smoothstep(0.4, 0.6, dot(under, float3(0.2126, 0.7152, 0.0722))));
    }

    // The distance fade.
    const float fadeEnd = min(grid.params.y, input.eye.w * kFarFraction);
    const float fadeStart = min(grid.params.x, fadeEnd);
    const float f = saturate((length(input.worldPos - input.eye.xyz) - fadeStart) / max(fadeEnd - fadeStart, 1e-5));

    // The glancing angle fade.
    const float grazing = 1.0 - pow(1.0 - abs(dot(normalize(input.toEye), normalize(input.normal))), 3.0);
    const float coverage = line * (1.0 - f * f * (3.0 - 2.0 * f)) * grazing;
    const float alpha = coverage * color.a;
    if (alpha <= epsilon) {
        discard;
    }

    if (gridSwitch(kGridDepth)) {
        // Depth where the coverage beats the noise, or half.
        const float threshold = gridSwitch(kGridDepthDither) ? gridNoise(floor(input.position.xy)) : 0.5;
        if (coverage < threshold) {
            discard;
        }
        return float4(0.0);
    }

    // The colours are gamma encoded: a linear HDR target takes them decoded.
    const float3 rgb = grid.output.y > 0.5 ? pow(max(color.rgb, float3(0.0)), float3(2.2)) : color.rgb;
    return float4(rgb, alpha);
}
