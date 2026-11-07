// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// GPU instance culling, second dispatch (one thread per instance): tests each instance's
// bounding sphere against six planes and appends the visible ones to the output with an
// atomic add on the instance count, which the indirect draw then reads.
//
// The `occlusion` variant also tests what the planes keep against a camera's Hi-Z pyramid
// (`vtscenemaps`, attached with Compute::setHiZMap): the box around the bounding sphere is
// projected with params.occlusionMatrix, the level is picked where its rectangle spans at most
// 2x2 texels, and the instance is dropped when its nearest depth lies beyond the farthest depth
// of every texel the rectangle touches. Everything uncertain is kept: a box crossing the near
// plane, a rectangle outside the depth's viewport, no pyramid.
//
// @variant frustum: CULL_FRUSTUM=1
// @variant occlusion: CULL_OCCLUSION=1
import vtdispatch;
import vtinstancecull;
#if defined(CULL_OCCLUSION)
import vtscenemaps;

// Whether the box of half-size r around c (the instances' space) is hidden in the pyramid.
bool occluded(float3 c, float r)
{
    const uint levels = sceneHiZLevels();
    if (levels == 0u) {
        return false;
    }
    float2 ndcMin = float2(1.0e30, 1.0e30);
    float2 ndcMax = float2(-1.0e30, -1.0e30);
    float nearest = 1.0;
    for (int i = 0; i < 8; ++i) {
        const float3 corner = c + float3((i & 1) != 0 ? r : -r, (i & 2) != 0 ? r : -r, (i & 4) != 0 ? r : -r);
        const float4 clip = mul(float4(corner, 1.0), params.occlusionMatrix);
        // at or behind the near plane: the box crosses it, and is kept
        if (clip.w <= 0.0 || clip.z < -clip.w) {
            return false;
        }
        const float3 ndc = clip.xyz / clip.w;
        ndcMin = min(ndcMin, ndc.xy);
        ndcMax = max(ndcMax, ndc.xy);
        // the depth buffer's value: GL clip z remapped to [0, 1], as every vertex stage does
        nearest = min(nearest, ndc.z * 0.5 + 0.5);
    }

    // the rectangle in depth texels, row 0 at the top (NDC +y)
    const float4 viewport = float4(sceneHiZViewport());
    const float2 lo = viewport.xy + float2(ndcMin.x * 0.5 + 0.5, 0.5 - ndcMax.y * 0.5) * viewport.zw;
    const float2 hi = viewport.xy + float2(ndcMax.x * 0.5 + 0.5, 0.5 - ndcMin.y * 0.5) * viewport.zw;
    const float2 viewportLast = viewport.xy + viewport.zw - 1.0;
    if (any(hi < viewport.xy) || any(lo > viewportLast)) {
        return false;
    }
    const int2 first = int2(floor(clamp(lo, viewport.xy, viewportLast)));
    const int2 last = int2(floor(clamp(hi, viewport.xy, viewportLast)));

    // the level whose texels (2^(level + 1) depth texels wide) leave at most 2 per axis
    const int extent = max(last.x - first.x, last.y - first.y);
    int level = 0;
    while (level + 1 < int(levels) && (1 << (level + 1)) <= extent) {
        ++level;
    }
    const int2 size = int2(sceneHiZSize(level));
    const int2 a = min(first >> (level + 1), size - 1);
    const int2 b = min(last >> (level + 1), size - 1);
    float farthest = 0.0;
    for (int y = a.y; y <= b.y; ++y) {
        for (int x = a.x; x <= b.x; ++x) {
            farthest = max(farthest, sceneHiZLoad(int2(x, y), level).y);
        }
    }
    return nearest > farthest;
}
#endif

[shader("compute")]
[numthreads(64, 1, 1)]
void instanceCull(uint3 tid : SV_DispatchThreadID)
{
    // A thread per instance, spread over rows of workgroups past the per-dimension limit.
    const uint4 dispatch = uint4(params.instanceCount, 1, 1, params.dispatchRowThreads);
    if (!dispatchThreadInRange(tid, dispatch)) {
        return;
    }
    const uint index = dispatchThreadIndex(tid, dispatch);
    const float3 center = input[index].columns[3].xyz;
    for (int p = 0; p < 6; ++p) {
        const float4 plane = params.frustumPlanes[p];
        if (dot(plane.xyz, center) + plane.w < -params.boundingSphereRadius) {
            return;
        }
    }
#if defined(CULL_OCCLUSION)
    if (params.occlusion.x != 0.0 && occluded(center, params.occlusion.y)) {
        return;
    }
#endif
    uint slot;
    InterlockedAdd(args[1], 1u, slot);
    output[slot] = input[index];
}
