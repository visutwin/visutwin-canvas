// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The wide-line shader, in both languages (upstream extras/renderers/wide-line-renderer.js).
//
// One instance per SEGMENT. The template geometry is generated from the vertex id
// rather than read from a vertex buffer, the way the engine's other storage draws
// work: a quad for the segment body, two discs for round caps and joins, and two
// triangles for bevel joins. The `kind` component of each template vertex says
// which, and a piece that is not wanted for the current style collapses to zero
// size rather than being skipped, so every instance draws the same vertex count.
#pragma once

namespace visutwin::canvas::wideline
{
    // Template layout, shared by both languages and by the C++ that sizes the draw:
    //   0 ..  5   quad body        (kind 0)
    //   6 .. 53   start disc       (kind 1)  16 triangles
    //  54 ..101   end disc         (kind 2)  16 triangles
    // 102 ..107   bevel triangles  (kinds 3/4/5)
    inline constexpr int kRoundSegments = 16;
    inline constexpr int kTemplateVertices = 6 + 2 * (kRoundSegments * 3) + 6;

    constexpr const char* WIDE_LINE_MSL = R"(
#include <metal_stdlib>
using namespace metal;

struct Segment {
    float4 prevWidth;      // prev.xyz,  startWidth
    float4 startWidth;     // start.xyz, endWidth
    float4 endDistance;    // end.xyz,   startDistance
    float4 nextDistance;   // next.xyz,  endDistance
    float4 startColor;     // rgb, unused
    float4 endColor;       // rgb, unused
    float4 style;          // join, cap, dashLength, gapLength
    float4 dashFlags;      // dashOffset, flags, worldSpaceWidth, unused
};

struct SceneData { float4x4 projViewMatrix; };
struct LineParams { float4 screenSize; };   // width, height, 1/width, 1/height

struct Varyings {
    float4 position [[position]];
    float3 color;
    float4 dash;
    float3 lineData;
    float distance;
};

constant int kRoundSegments = 16;

// The template vertex for this vertex id, as (x, y, kind).
static float3 templateVertex(uint vid)
{
    if (vid < 6u) {
        const float2 quad[6] = {
            float2(0.0, -1.0), float2(1.0, -1.0), float2(0.0, 1.0),
            float2(0.0,  1.0), float2(1.0, -1.0), float2(1.0, 1.0)
        };
        return float3(quad[vid], 0.0);
    }
    const uint discVertices = uint(kRoundSegments) * 3u;
    if (vid < 6u + discVertices * 2u) {
        const uint local = vid - 6u;
        const float kind = local < discVertices ? 1.0 : 2.0;
        const uint inDisc = local % discVertices;
        const uint triangle = inDisc / 3u;
        const uint corner = inDisc % 3u;
        if (corner == 0u) {
            return float3(0.0, 0.0, kind);
        }
        const float step = 6.28318530718 / float(kRoundSegments);
        const float angle = float(triangle + corner - 1u) * step;
        return float3(cos(angle), sin(angle), kind);
    }
    // Two bevel triangles, one per side: the corner point plus the two edges.
    const uint local = vid - 6u - discVertices * 2u;
    const float side = local < 3u ? -1.0 : 1.0;
    const uint corner = local % 3u;
    if (corner == 0u) { return float3(0.0, 0.0, 3.0); }
    return float3(0.0, side, corner == 1u ? 4.0 : 5.0);
}

static float2 safeNormalize(float2 value, float2 fallback)
{
    const float len = length(value);
    return len > 0.00001 ? value / len : fallback;
}

static float2 perpendicular(float2 value) { return float2(-value.y, value.x); }

static float2 toScreen(float4 clipPosition, float4 screenSize)
{
    const float w = abs(clipPosition.w) > 0.00001 ? clipPosition.w : 0.00001;
    return clipPosition.xy / w * screenSize.xy * 0.5;
}

static float4 offsetClip(float4 clipPosition, float2 pixelOffset, float4 screenSize)
{
    clipPosition.xy += pixelOffset * (2.0 * screenSize.zw) * clipPosition.w;
    return clipPosition;
}

static float resolveHalfWidth(float width, float4 clipPosition, float worldSpace,
                              float4x4 projView, float4 screenSize)
{
    if (worldSpace > 0.5) {
        // A world-space width has to become pixels before the screen-space
        // expansion below can use it.
        const float pixelsPerWorldUnit = abs(projView[1][1]) * screenSize.y * 0.5 /
            max(abs(clipPosition.w), 0.00001);
        return width * 0.5 * pixelsPerWorldUnit;
    }
    return width * 0.5;
}

static float2 miterOffset(float2 firstDirection, float2 secondDirection, float side,
                          float halfWidth)
{
    const float2 secondNormal = perpendicular(secondDirection);
    const float2 miter = safeNormalize(perpendicular(firstDirection) + secondNormal, secondNormal);
    // A very sharp corner would send the miter to infinity, so it is clamped.
    const float scale = halfWidth / max(dot(miter, secondNormal), 0.25);
    return miter * side * min(scale, halfWidth * 4.0);
}

vertex Varyings wideLineVS(uint vid [[vertex_id]],
                           uint iid [[instance_id]],
                           constant SceneData& scene       [[buffer(1)]],
                           const device Segment* segments  [[buffer(7)]],
                           constant LineParams& params     [[buffer(11)]])
{
    const Segment seg = segments[iid];
    const float4 screenSize = params.screenSize;

    const float4 prevClip = scene.projViewMatrix * float4(seg.prevWidth.xyz, 1.0);
    const float4 startClip = scene.projViewMatrix * float4(seg.startWidth.xyz, 1.0);
    const float4 endClip = scene.projViewMatrix * float4(seg.endDistance.xyz, 1.0);
    const float4 nextClip = scene.projViewMatrix * float4(seg.nextDistance.xyz, 1.0);

    const float worldSpace = seg.dashFlags.z;
    const float startHalfWidth =
        resolveHalfWidth(seg.prevWidth.w, startClip, worldSpace, scene.projViewMatrix, screenSize);
    const float endHalfWidth =
        resolveHalfWidth(seg.startWidth.w, endClip, worldSpace, scene.projViewMatrix, screenSize);

    const float2 startScreen = toScreen(startClip, screenSize);
    const float2 endScreen = toScreen(endClip, screenSize);
    const float2 currentDirection = safeNormalize(endScreen - startScreen, float2(1.0, 0.0));
    const float2 previousDirection =
        safeNormalize(startScreen - toScreen(prevClip, screenSize), currentDirection);
    const float2 nextDirection =
        safeNormalize(toScreen(nextClip, screenSize) - endScreen, currentDirection);
    const float2 currentNormal = perpendicular(currentDirection);
    const float2 previousNormal = perpendicular(previousDirection);

    const float joinStyle = seg.style.x;
    const float capStyle = seg.style.y;
    const uint flags = uint(seg.dashFlags.y + 0.5);
    const bool startConnected = (flags & 1u) != 0u;
    const bool endConnected = (flags & 2u) != 0u;

    const float3 tmpl = templateVertex(vid);
    const float kind = tmpl.z;
    float4 clipPosition;

    Varyings out;
    out.lineData = float3(0.0, startHalfWidth, 0.0);

    if (kind < 0.5) {
        const float along = tmpl.x;
        const float side = tmpl.y;
        const bool atStart = along < 0.5;
        const float halfWidth = atStart ? startHalfWidth : endHalfWidth;
        float2 offset;

        if (atStart) {
            if (startConnected && joinStyle < 0.5) {
                offset = miterOffset(previousDirection, currentDirection, side, halfWidth);
            } else {
                offset = currentNormal * side * halfWidth;
                if (!startConnected && capStyle > 0.5 && capStyle < 1.5) {
                    offset -= currentDirection * halfWidth;
                }
            }
            clipPosition = offsetClip(startClip, offset, screenSize);
        } else {
            if (endConnected && joinStyle < 0.5) {
                offset = miterOffset(currentDirection, nextDirection, side, halfWidth);
            } else {
                offset = currentNormal * side * halfWidth;
                if (!endConnected && capStyle > 0.5 && capStyle < 1.5) {
                    offset += currentDirection * halfWidth;
                }
            }
            clipPosition = offsetClip(endClip, offset, screenSize);
        }

        out.color = mix(seg.startColor.rgb, seg.endColor.rgb, along);
        out.distance = mix(seg.endDistance.w, seg.nextDistance.w, along);
        out.lineData = float3(side, mix(startHalfWidth, endHalfWidth, along), 1.0);
    } else if (kind < 1.5) {
        const bool visible = (startConnected && joinStyle > 1.5) || (!startConnected && capStyle > 1.5);
        const float scale = visible ? startHalfWidth : 0.0;
        const float2 offset = (currentDirection * tmpl.x + currentNormal * tmpl.y) * scale;
        clipPosition = offsetClip(startClip, offset, screenSize);
        out.color = seg.startColor.rgb;
        out.distance = seg.endDistance.w;
    } else if (kind < 2.5) {
        const bool visible = !endConnected && capStyle > 1.5;
        const float scale = visible ? endHalfWidth : 0.0;
        const float2 offset = (currentDirection * tmpl.x + currentNormal * tmpl.y) * scale;
        clipPosition = offsetClip(endClip, offset, screenSize);
        out.color = seg.endColor.rgb;
        out.distance = seg.nextDistance.w;
    } else {
        const bool visible = startConnected && joinStyle > 0.5 && joinStyle < 1.5;
        const float scale = visible ? startHalfWidth : 0.0;
        float2 offset = float2(0.0);
        if (kind > 3.5 && kind < 4.5) {
            offset = previousNormal * tmpl.y * scale;
        } else if (kind > 4.5) {
            offset = currentNormal * tmpl.y * scale;
        }
        clipPosition = offsetClip(startClip, offset, screenSize);
        out.color = seg.startColor.rgb;
        out.distance = seg.endDistance.w;
    }

    out.dash = float4(seg.style.z, seg.style.w, seg.dashFlags.x, capStyle);
    // Standard [0,1] depth, the same remap every engine vertex shader applies.
    clipPosition.z = 0.5 * (clipPosition.z + clipPosition.w);
    out.position = clipPosition;
    return out;
}

fragment float4 wideLineFS(Varyings in [[stage_in]])
{
    if (in.dash.x > 0.0 && in.dash.y > 0.0) {
        const float period = in.dash.x + in.dash.y;
        float phase = fmod(in.distance + in.dash.z, period);
        if (phase < 0.0) { phase += period; }
        if (phase > in.dash.x) {
            discard_fragment();
        }
    }
    return float4(in.color, 1.0);
}
)";

    constexpr const char* WIDE_LINE_GLSL = R"(#version 450

#ifdef VT_VERTEX_SHADER
layout(push_constant) uniform PushConstants { mat4 viewProjection; mat4 model; } pc;
struct Segment {
    vec4 prevWidth; vec4 startWidth; vec4 endDistance; vec4 nextDistance;
    vec4 startColor; vec4 endColor; vec4 style; vec4 dashFlags;
};
layout(set = 6, binding = 0, std430) readonly buffer Segments { Segment values[]; } segments;
layout(set = 6, binding = 3, std140) uniform LineParams { vec4 screenSize; } params;

layout(location = 0) out vec3 vColor;
layout(location = 1) out vec4 vDash;
layout(location = 2) out vec3 vLineData;
layout(location = 3) out float vDistance;

const int kRoundSegments = 16;

vec3 templateVertex(uint vid) {
    if (vid < 6u) {
        vec2 quad[6] = vec2[6](
            vec2(0.0, -1.0), vec2(1.0, -1.0), vec2(0.0, 1.0),
            vec2(0.0,  1.0), vec2(1.0, -1.0), vec2(1.0, 1.0));
        return vec3(quad[vid], 0.0);
    }
    uint discVertices = uint(kRoundSegments) * 3u;
    if (vid < 6u + discVertices * 2u) {
        uint local = vid - 6u;
        float kind = local < discVertices ? 1.0 : 2.0;
        uint inDisc = local % discVertices;
        uint tri = inDisc / 3u;
        uint corner = inDisc % 3u;
        if (corner == 0u) { return vec3(0.0, 0.0, kind); }
        float step = 6.28318530718 / float(kRoundSegments);
        float angle = float(tri + corner - 1u) * step;
        return vec3(cos(angle), sin(angle), kind);
    }
    uint local = vid - 6u - discVertices * 2u;
    float side = local < 3u ? -1.0 : 1.0;
    uint corner = local % 3u;
    if (corner == 0u) { return vec3(0.0, 0.0, 3.0); }
    return vec3(0.0, side, corner == 1u ? 4.0 : 5.0);
}

vec2 safeNormalize(vec2 value, vec2 fallback) {
    float len = length(value);
    return len > 0.00001 ? value / len : fallback;
}

vec2 perpendicular(vec2 value) { return vec2(-value.y, value.x); }

vec2 toScreen(vec4 clipPosition, vec4 screenSize) {
    float w = abs(clipPosition.w) > 0.00001 ? clipPosition.w : 0.00001;
    return clipPosition.xy / w * screenSize.xy * 0.5;
}

vec4 offsetClip(vec4 clipPosition, vec2 pixelOffset, vec4 screenSize) {
    clipPosition.xy += pixelOffset * (2.0 * screenSize.zw) * clipPosition.w;
    return clipPosition;
}

float resolveHalfWidth(float width, vec4 clipPosition, float worldSpace, vec4 screenSize) {
    if (worldSpace > 0.5) {
        float pixelsPerWorldUnit = abs(pc.viewProjection[1][1]) * screenSize.y * 0.5 /
            max(abs(clipPosition.w), 0.00001);
        return width * 0.5 * pixelsPerWorldUnit;
    }
    return width * 0.5;
}

vec2 miterOffset(vec2 firstDirection, vec2 secondDirection, float side, float halfWidth) {
    vec2 secondNormal = perpendicular(secondDirection);
    vec2 miter = safeNormalize(perpendicular(firstDirection) + secondNormal, secondNormal);
    float scale = halfWidth / max(dot(miter, secondNormal), 0.25);
    return miter * side * min(scale, halfWidth * 4.0);
}

void main() {
    Segment seg = segments.values[gl_InstanceIndex];
    vec4 screenSize = params.screenSize;

    vec4 prevClip = pc.viewProjection * vec4(seg.prevWidth.xyz, 1.0);
    vec4 startClip = pc.viewProjection * vec4(seg.startWidth.xyz, 1.0);
    vec4 endClip = pc.viewProjection * vec4(seg.endDistance.xyz, 1.0);
    vec4 nextClip = pc.viewProjection * vec4(seg.nextDistance.xyz, 1.0);

    float worldSpace = seg.dashFlags.z;
    float startHalfWidth = resolveHalfWidth(seg.prevWidth.w, startClip, worldSpace, screenSize);
    float endHalfWidth = resolveHalfWidth(seg.startWidth.w, endClip, worldSpace, screenSize);

    vec2 startScreen = toScreen(startClip, screenSize);
    vec2 endScreen = toScreen(endClip, screenSize);
    vec2 currentDirection = safeNormalize(endScreen - startScreen, vec2(1.0, 0.0));
    vec2 previousDirection =
        safeNormalize(startScreen - toScreen(prevClip, screenSize), currentDirection);
    vec2 nextDirection = safeNormalize(toScreen(nextClip, screenSize) - endScreen, currentDirection);
    vec2 currentNormal = perpendicular(currentDirection);
    vec2 previousNormal = perpendicular(previousDirection);

    float joinStyle = seg.style.x;
    float capStyle = seg.style.y;
    uint flags = uint(seg.dashFlags.y + 0.5);
    bool startConnected = (flags & 1u) != 0u;
    bool endConnected = (flags & 2u) != 0u;

    vec3 tmpl = templateVertex(uint(gl_VertexIndex));
    float kind = tmpl.z;
    vec4 clipPosition;
    vLineData = vec3(0.0, startHalfWidth, 0.0);

    if (kind < 0.5) {
        float along = tmpl.x;
        float side = tmpl.y;
        bool atStart = along < 0.5;
        float halfWidth = atStart ? startHalfWidth : endHalfWidth;
        vec2 offset;
        if (atStart) {
            if (startConnected && joinStyle < 0.5) {
                offset = miterOffset(previousDirection, currentDirection, side, halfWidth);
            } else {
                offset = currentNormal * side * halfWidth;
                if (!startConnected && capStyle > 0.5 && capStyle < 1.5) {
                    offset -= currentDirection * halfWidth;
                }
            }
            clipPosition = offsetClip(startClip, offset, screenSize);
        } else {
            if (endConnected && joinStyle < 0.5) {
                offset = miterOffset(currentDirection, nextDirection, side, halfWidth);
            } else {
                offset = currentNormal * side * halfWidth;
                if (!endConnected && capStyle > 0.5 && capStyle < 1.5) {
                    offset += currentDirection * halfWidth;
                }
            }
            clipPosition = offsetClip(endClip, offset, screenSize);
        }
        vColor = mix(seg.startColor.rgb, seg.endColor.rgb, along);
        vDistance = mix(seg.endDistance.w, seg.nextDistance.w, along);
        vLineData = vec3(side, mix(startHalfWidth, endHalfWidth, along), 1.0);
    } else if (kind < 1.5) {
        bool visible = (startConnected && joinStyle > 1.5) || (!startConnected && capStyle > 1.5);
        float scale = visible ? startHalfWidth : 0.0;
        vec2 offset = (currentDirection * tmpl.x + currentNormal * tmpl.y) * scale;
        clipPosition = offsetClip(startClip, offset, screenSize);
        vColor = seg.startColor.rgb;
        vDistance = seg.endDistance.w;
    } else if (kind < 2.5) {
        bool visible = !endConnected && capStyle > 1.5;
        float scale = visible ? endHalfWidth : 0.0;
        vec2 offset = (currentDirection * tmpl.x + currentNormal * tmpl.y) * scale;
        clipPosition = offsetClip(endClip, offset, screenSize);
        vColor = seg.endColor.rgb;
        vDistance = seg.nextDistance.w;
    } else {
        bool visible = startConnected && joinStyle > 0.5 && joinStyle < 1.5;
        float scale = visible ? startHalfWidth : 0.0;
        vec2 offset = vec2(0.0);
        if (kind > 3.5 && kind < 4.5) {
            offset = previousNormal * tmpl.y * scale;
        } else if (kind > 4.5) {
            offset = currentNormal * tmpl.y * scale;
        }
        clipPosition = offsetClip(startClip, offset, screenSize);
        vColor = seg.startColor.rgb;
        vDistance = seg.endDistance.w;
    }

    vDash = vec4(seg.style.z, seg.style.w, seg.dashFlags.x, capStyle);
    // Standard [0,1] depth: the engine's vertex shaders remap clip.z from GL's
    // [-1,1], and a user shader that skips it wins every depth test.
    clipPosition.z = 0.5 * (clipPosition.z + clipPosition.w);
    gl_Position = clipPosition;
}
#endif

#ifdef VT_FRAGMENT_SHADER
layout(location = 0) in vec3 vColor;
layout(location = 1) in vec4 vDash;
layout(location = 2) in vec3 vLineData;
layout(location = 3) in float vDistance;
layout(location = 0) out vec4 outColor;

void main() {
    if (vDash.x > 0.0 && vDash.y > 0.0) {
        float period = vDash.x + vDash.y;
        float phase = mod(vDistance + vDash.z, period);
        if (phase < 0.0) { phase += period; }
        if (phase > vDash.x) { discard; }
    }
    outColor = vec4(vColor, 1.0);
}
#endif
)";
}
