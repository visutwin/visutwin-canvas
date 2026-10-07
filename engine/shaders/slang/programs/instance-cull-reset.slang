// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// GPU instance culling, first dispatch (one thread): writes the mesh's index range into the
// indirect draw arguments and zeroes their instance count. ComputeInstanceCuller runs it
// before instance-cull over the generic Compute seam.
import vtinstancecull;

[shader("compute")]
[numthreads(1, 1, 1)]
void instanceCullReset(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x != 0u) {
        return;
    }
    args[0] = params.indexCount;
    args[1] = 0u;
    args[2] = params.indexStart;
    args[3] = asuint(params.baseVertex);
    args[4] = params.baseInstance;
}
