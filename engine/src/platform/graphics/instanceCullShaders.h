// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The GPU instance-cull kernels, MSL and GLSL, run over the generic Compute seam by
// ComputeInstanceCuller. Two dispatches a cull: RESET (one thread) writes the mesh's index
// range into the indirect draw arguments and zeroes their instance count; CULL (one thread
// per instance) tests each instance's bounding sphere against six planes and appends the
// visible ones to the output with an atomic add on that count, which the indirect draw
// then reads as its instance count.
//
// Bindings follow Compute's name-order contract: buffers "args", "input", "output" at
// 0, 1, 2 and the uniform block (InstanceCullParams, 128 bytes) after them, buffer(3) on
// Metal and binding 3 on Vulkan. An instance is the 80-byte InstanceData record (model
// matrix + colour); the draw arguments are the 20-byte indexed-indirect layout both APIs
// share (index count, instance count, first index, vertex offset, first instance).
//
#pragma once

#include <cstdint>

namespace visutwin::canvas::instance_cull_shaders
{
    constexpr uint32_t kCullThreads = 64u;

    constexpr const char* INSTANCE_CULL_MSL = R"(
#include <metal_stdlib>
using namespace metal;

struct CullParams {
    float4 frustumPlanes[6];    // (nx, ny, nz, d); dot(n, p) + d >= 0 is inside
    float  boundingSphereRadius;
    uint   instanceCount;
    uint   indexCount;
    uint   indexStart;
    int    baseVertex;
    uint   baseInstance;
    float  pad[2];
};

struct InstanceData {
    float4x4 modelMatrix;
    float4   diffuseColor;
};

struct DrawArgs {
    uint        indexCount;
    atomic_uint instanceCount;
    uint        indexStart;
    int         baseVertex;
    uint        baseInstance;
};

kernel void instanceCullReset(
    device DrawArgs*      args   [[buffer(0)]],
    constant CullParams&  params [[buffer(3)]],
    uint                  tid    [[thread_position_in_grid]])
{
    if (tid != 0) return;
    args->indexCount = params.indexCount;
    atomic_store_explicit(&args->instanceCount, 0u, memory_order_relaxed);
    args->indexStart = params.indexStart;
    args->baseVertex = params.baseVertex;
    args->baseInstance = params.baseInstance;
}

kernel void instanceCull(
    device DrawArgs*            args   [[buffer(0)]],
    device const InstanceData*  input  [[buffer(1)]],
    device InstanceData*        output [[buffer(2)]],
    constant CullParams&        params [[buffer(3)]],
    uint                        tid    [[thread_position_in_grid]])
{
    if (tid >= params.instanceCount) return;
    const float3 center = input[tid].modelMatrix[3].xyz;
    for (int p = 0; p < 6; ++p) {
        const float4 plane = params.frustumPlanes[p];
        if (dot(plane.xyz, center) + plane.w < -params.boundingSphereRadius) {
            return;
        }
    }
    const uint slot = atomic_fetch_add_explicit(&args->instanceCount, 1u, memory_order_relaxed);
    output[slot] = input[tid];
}
)";

    // The declarations both GLSL kernels share.
#define VT_INSTANCE_CULL_GLSL_DECLARATIONS R"(#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct InstanceData {
    mat4 modelMatrix;
    vec4 diffuseColor;
};

layout(std430, set = 0, binding = 0) buffer DrawArgs {
    uint indexCount;
    uint instanceCount;
    uint indexStart;
    int baseVertex;
    uint baseInstance;
} args;
layout(std430, set = 0, binding = 1) readonly buffer InputInstances {
    InstanceData instances[];
} inputInstances;
layout(std430, set = 0, binding = 2) writeonly buffer OutputInstances {
    InstanceData instances[];
} outputInstances;
layout(std140, set = 0, binding = 3) uniform CullParams {
    vec4 frustumPlanes[6];
    float boundingSphereRadius;
    uint instanceCount;
    uint indexCount;
    uint indexStart;
    int baseVertex;
    uint baseInstance;
    vec2 pad;
} params;
)"

    constexpr const char* INSTANCE_CULL_RESET_GLSL = VT_INSTANCE_CULL_GLSL_DECLARATIONS R"(
void main()
{
    if (gl_GlobalInvocationID.x != 0u) return;
    args.indexCount = params.indexCount;
    args.instanceCount = 0u;
    args.indexStart = params.indexStart;
    args.baseVertex = params.baseVertex;
    args.baseInstance = params.baseInstance;
}
)";

    constexpr const char* INSTANCE_CULL_GLSL = VT_INSTANCE_CULL_GLSL_DECLARATIONS R"(
void main()
{
    uint sourceIndex = gl_GlobalInvocationID.x;
    if (sourceIndex >= params.instanceCount) return;
    vec3 center = inputInstances.instances[sourceIndex].modelMatrix[3].xyz;
    for (uint p = 0u; p < 6u; ++p) {
        vec4 plane = params.frustumPlanes[p];
        if (dot(plane.xyz, center) + plane.w < -params.boundingSphereRadius) {
            return;
        }
    }
    uint slot = atomicAdd(args.instanceCount, 1u);
    outputInstances.instances[slot] = inputInstances.instances[sourceIndex];
}
)";

#undef VT_INSTANCE_CULL_GLSL_DECLARATIONS
}
