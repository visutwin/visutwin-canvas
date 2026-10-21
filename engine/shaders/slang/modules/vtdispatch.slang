// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.10.2026
//
// The threads of a dispatch set up with Compute::setupDispatchThreads. The dispatch is rounded
// up to whole workgroups, so a kernel skips the threads past the count it was set up for; and a
// dispatch in one dimension that needs more workgroups than one dimension holds is spread over
// rows of them, which these functions account for. The kernel receives the dispatch in its own
// uniform block, since a kernel's block is its own here, as ONE uint4
// (Compute::dispatchThreads(): the thread count in xyz, the threads in a row in w), and passes
// it in:
//
//     if (!dispatchThreadInRange(id, params.dispatch)) { return; }
//     const uint index = dispatchThreadIndex(id, params.dispatch);
//
// One uint4, not a uint3 and a uint: in a constant block a 3-vector takes 16 bytes on Metal and
// 12 under Vulkan's std140, so a scalar after it lands at different offsets on the two.
module vtdispatch;

/// The linear index of the thread, x varying fastest.
public uint dispatchThreadIndex(uint3 globalId, uint4 dispatch)
{
    return (globalId.z * dispatch.y + globalId.y) * dispatch.w + globalId.x;
}

/// True when the thread is one of the threads the dispatch was set up for.
public bool dispatchThreadInRange(uint3 globalId, uint4 dispatch)
{
    // Spread over rows of workgroups, a one-dimensional dispatch is rounded up to whole rows.
    if (dispatch.w != dispatch.x) {
        return dispatchThreadIndex(globalId, dispatch) < dispatch.x;
    }
    return all(globalId < dispatch.xyz);
}
