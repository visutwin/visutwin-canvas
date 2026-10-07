// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The wide-line shader: one instance per SEGMENT. The template geometry is generated from
// the vertex id rather than read from a vertex buffer, the way the engine's other storage
// draws work: a quad for the segment body, two discs for round caps and joins, and two
// triangles for bevel joins. The kind of each template vertex says which, and a piece not
// wanted for the current style collapses to zero size rather than being skipped, so every
// instance draws the same vertex count. Template layout (also sized by the C++,
// wideLineShaders.h):
//   0 ..  5   quad body        (kind 0)
//   6 .. 53   start disc       (kind 1)  16 triangles
//  54 ..101   end disc         (kind 2)  16 triangles
// 102 ..107   bevel triangles  (kinds 3/4/5)
// Bindings, a storage draw's: the segments at Metal buffer 7 / Vulkan set 6 binding 0, the
// line parameters at buffer 11 / set 6 binding 3, the view-projection through vtdraw.
import vtdraw;

struct Segment
{
    float4 prevWidth;      // prev.xyz,  startWidth
    float4 startWidth;     // start.xyz, endWidth
    float4 endDistance;    // end.xyz,   startDistance
    float4 nextDistance;   // next.xyz,  endDistance
    float4 startColor;     // rgb, opacity
    float4 endColor;       // rgb, opacity
    float4 style;          // join, cap, dashLength, gapLength
    float4 dashFlags;      // dashOffset, flags, worldSpaceWidth, unused
};

struct LineParams
{
    float4 screenSize;     // width, height, 1/width, 1/height
};

[[vk::binding(0, 6)]] StructuredBuffer<Segment> segments : register(t7);
[[vk::binding(3, 6)]] ConstantBuffer<LineParams> params : register(b11);

struct Varyings
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float4 dash : TEXCOORD0;
    float3 lineData : TEXCOORD1;
    float distance : TEXCOORD2;
};

static const int kRoundSegments = 16;

// The template vertex for this vertex id, as (x, y, kind).
float3 templateVertex(uint vid)
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
        const float stepAngle = 6.28318530718 / float(kRoundSegments);
        const float angle = float(triangle + corner - 1u) * stepAngle;
        return float3(cos(angle), sin(angle), kind);
    }
    // Two bevel triangles, one per side: the corner point plus the two edges.
    const uint local = vid - 6u - discVertices * 2u;
    const float side = local < 3u ? -1.0 : 1.0;
    const uint corner = local % 3u;
    if (corner == 0u) {
        return float3(0.0, 0.0, 3.0);
    }
    return float3(0.0, side, corner == 1u ? 4.0 : 5.0);
}

float2 safeNormalize(float2 value, float2 fallback)
{
    const float len = length(value);
    return len > 0.00001 ? value / len : fallback;
}

float2 perpendicular(float2 value) { return float2(-value.y, value.x); }

float2 toScreen(float4 clipPosition, float4 screenSize)
{
    const float w = abs(clipPosition.w) > 0.00001 ? clipPosition.w : 0.00001;
    return clipPosition.xy / w * screenSize.xy * 0.5;
}

float4 offsetClip(float4 clipPosition, float2 pixelOffset, float4 screenSize)
{
    clipPosition.xy += pixelOffset * (2.0 * screenSize.zw) * clipPosition.w;
    return clipPosition;
}

// A world-space width has to become pixels before the screen-space expansion can use it.
float resolveHalfWidth(float width, float4 clipPosition, float worldSpace, float4x4 projView, float4 screenSize)
{
    if (worldSpace > 0.5) {
        const float pixelsPerWorldUnit = abs(projView[1][1]) * screenSize.y * 0.5 / max(abs(clipPosition.w), 0.00001);
        return width * 0.5 * pixelsPerWorldUnit;
    }
    return width * 0.5;
}

// A very sharp corner would send the miter to infinity, so it is clamped.
float2 miterOffset(float2 firstDirection, float2 secondDirection, float side, float halfWidth)
{
    const float2 secondNormal = perpendicular(secondDirection);
    const float2 miter = safeNormalize(perpendicular(firstDirection) + secondNormal, secondNormal);
    const float scale = halfWidth / max(dot(miter, secondNormal), 0.25);
    return miter * side * min(scale, halfWidth * 4.0);
}

[shader("vertex")]
Varyings wideLineVS(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    const Segment seg = segments[iid];
    const float4 screenSize = params.screenSize;
    const float4x4 projView = drawViewProjection();

    const float4 prevClip = mul(float4(seg.prevWidth.xyz, 1.0), projView);
    const float4 startClip = mul(float4(seg.startWidth.xyz, 1.0), projView);
    const float4 endClip = mul(float4(seg.endDistance.xyz, 1.0), projView);
    const float4 nextClip = mul(float4(seg.nextDistance.xyz, 1.0), projView);

    const float worldSpace = seg.dashFlags.z;
    const float startHalfWidth = resolveHalfWidth(seg.prevWidth.w, startClip, worldSpace, projView, screenSize);
    const float endHalfWidth = resolveHalfWidth(seg.startWidth.w, endClip, worldSpace, projView, screenSize);

    const float2 startScreen = toScreen(startClip, screenSize);
    const float2 endScreen = toScreen(endClip, screenSize);
    const float2 currentDirection = safeNormalize(endScreen - startScreen, float2(1.0, 0.0));
    const float2 previousDirection = safeNormalize(startScreen - toScreen(prevClip, screenSize), currentDirection);
    const float2 nextDirection = safeNormalize(toScreen(nextClip, screenSize) - endScreen, currentDirection);
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

    Varyings output;
    output.lineData = float3(0.0, startHalfWidth, 0.0);

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
        output.color = lerp(seg.startColor, seg.endColor, along);
        output.distance = lerp(seg.endDistance.w, seg.nextDistance.w, along);
        output.lineData = float3(side, lerp(startHalfWidth, endHalfWidth, along), 1.0);
    } else if (kind < 1.5) {
        const bool visible = (startConnected && joinStyle > 1.5) || (!startConnected && capStyle > 1.5);
        const float scale = visible ? startHalfWidth : 0.0;
        const float2 offset = (currentDirection * tmpl.x + currentNormal * tmpl.y) * scale;
        clipPosition = offsetClip(startClip, offset, screenSize);
        output.color = seg.startColor;
        output.distance = seg.endDistance.w;
    } else if (kind < 2.5) {
        const bool visible = !endConnected && capStyle > 1.5;
        const float scale = visible ? endHalfWidth : 0.0;
        const float2 offset = (currentDirection * tmpl.x + currentNormal * tmpl.y) * scale;
        clipPosition = offsetClip(endClip, offset, screenSize);
        output.color = seg.endColor;
        output.distance = seg.nextDistance.w;
    } else {
        const bool visible = startConnected && joinStyle > 0.5 && joinStyle < 1.5;
        const float scale = visible ? startHalfWidth : 0.0;
        float2 offset = float2(0.0, 0.0);
        if (kind > 3.5 && kind < 4.5) {
            offset = previousNormal * tmpl.y * scale;
        } else if (kind > 4.5) {
            offset = currentNormal * tmpl.y * scale;
        }
        clipPosition = offsetClip(startClip, offset, screenSize);
        output.color = seg.startColor;
        output.distance = seg.endDistance.w;
    }

    output.dash = float4(seg.style.z, seg.style.w, seg.dashFlags.x, capStyle);
    // Standard [0, 1] depth, the same remap every engine vertex stage applies.
    clipPosition.z = 0.5 * (clipPosition.z + clipPosition.w);
    output.position = clipPosition;
    return output;
}

[shader("fragment")]
float4 wideLineFS(Varyings input) : SV_Target0
{
    if (input.dash.x > 0.0 && input.dash.y > 0.0) {
        const float period = input.dash.x + input.dash.y;
        float phase = fmod(input.distance + input.dash.z, period);
        if (phase < 0.0) {
            phase += period;
        }
        if (phase > input.dash.x) {
            discard;
        }
    }
    return input.color;
}
