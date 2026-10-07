// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// GPU instance culling, second dispatch (one thread per instance): tests each instance's
// bounding sphere against six planes and appends the visible ones to the output with an
// atomic add on the instance count, which the indirect draw then reads.
import vtinstancecull;

[shader("compute")]
[numthreads(64, 1, 1)]
void instanceCull(uint3 tid : SV_DispatchThreadID)
{
    const uint index = tid.x;
    if (index >= params.instanceCount) {
        return;
    }
    const float3 center = input[index].columns[3].xyz;
    for (int p = 0; p < 6; ++p) {
        const float4 plane = params.frustumPlanes[p];
        if (dot(plane.xyz, center) + plane.w < -params.boundingSphereRadius) {
            return;
        }
    }
    uint slot;
    InterlockedAdd(args[1], 1u, slot);
    output[slot] = input[index];
}
