// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Particle sorting (upstream PARTICLESORT_*), as one compute dispatch of ONE workgroup:
// each thread keys a share of the pool — upstream's keys, sorted ascending: minus the
// squared distance to the camera (farthest drawn first), the life (newest first) or minus
// the life (oldest first) — then the workgroup runs a bitonic sort over the pool rounded
// up to a power of two (padding keys sort last), with a barrier between stages, and
// writes the particle indices in draw order. One workgroup keeps every stage's barrier a
// workgroup barrier, so the sort is a single dispatch at any pool size; upstream sorts on
// the CPU instead, which on its GPU path it cannot.
//
// Bindings follow Compute's name-order contract: buffers "order", "particles",
// "sortKeys" at 0, 1, 2 and the uniform block after them (buffer(3) / binding 3).
//
#pragma once

#include <cstdint>

namespace visutwin::canvas::particle_sort_shaders
{
    struct alignas(16) SortUniforms
    {
        float cameraPosition[4];   // xyz the camera, in the particles' space; w the sort mode
        float counts[4];           // x particle count, y the power of two sorted over
    };
    static_assert(sizeof(SortUniforms) == 32);

    constexpr uint32_t kSortThreads = 256u;

    constexpr const char* PARTICLE_SORT_MSL = R"(
#include <metal_stdlib>
using namespace metal;

struct Particle {
    float4 posAge;
    float4 velLifetime;
    float4 rotSeedSize;
    float4 motion;
};

struct SortUniforms {
    float4 cameraPosition;
    float4 counts;
};

kernel void particleSortKernel(device uint* order [[buffer(0)]],
                               device const Particle* particles [[buffer(1)]],
                               device float2* keys [[buffer(2)]],
                               constant SortUniforms& u [[buffer(3)]],
                               uint tid [[thread_position_in_threadgroup]],
                               uint threads [[threads_per_threadgroup]])
{
    const uint count = uint(u.counts.x);
    const uint size = uint(u.counts.y);
    const uint mode = uint(u.cameraPosition.w + 0.5);

    for (uint i = tid; i < size; i += threads) {
        float key = INFINITY;
        if (i < count) {
            const Particle p = particles[i];
            if (mode == 1u) {
                const float3 d = p.posAge.xyz - u.cameraPosition.xyz;
                key = -dot(d, d);
            } else if (mode == 2u) {
                key = p.posAge.w;
            } else {
                key = -p.posAge.w;
            }
        }
        keys[i] = float2(key, float(i));
    }
    threadgroup_barrier(mem_flags::mem_device);

    for (uint k = 2u; k <= size; k <<= 1u) {
        for (uint j = k >> 1u; j > 0u; j >>= 1u) {
            for (uint i = tid; i < size; i += threads) {
                const uint partner = i ^ j;
                if (partner > i) {
                    const bool ascending = (i & k) == 0u;
                    const float2 a = keys[i];
                    const float2 b = keys[partner];
                    if ((a.x > b.x) == ascending) {
                        keys[i] = b;
                        keys[partner] = a;
                    }
                }
            }
            threadgroup_barrier(mem_flags::mem_device);
        }
    }

    for (uint i = tid; i < count; i += threads) {
        order[i] = uint(keys[i].y);
    }
}
)";

    constexpr const char* PARTICLE_SORT_GLSL = R"(
#version 450
layout(local_size_x = 256) in;
struct Particle { vec4 posAge; vec4 velLifetime; vec4 rotSeedSize; vec4 motion; };
layout(set = 0, binding = 0, std430) buffer Order { uint values[]; } order;
layout(set = 0, binding = 1, std430) readonly buffer Particles { Particle values[]; } particles;
layout(set = 0, binding = 2, std430) buffer Keys { vec2 values[]; } keys;
layout(set = 0, binding = 3, std140) uniform SortUniforms {
    vec4 cameraPosition;
    vec4 counts;
} u;

void main() {
    uint tid = gl_LocalInvocationID.x;
    uint threads = gl_WorkGroupSize.x;
    uint count = uint(u.counts.x);
    uint size = uint(u.counts.y);
    uint mode = uint(u.cameraPosition.w + 0.5);

    for (uint i = tid; i < size; i += threads) {
        float key = uintBitsToFloat(0x7F800000u);   // +infinity: padding sorts last
        if (i < count) {
            Particle p = particles.values[i];
            if (mode == 1u) {
                vec3 d = p.posAge.xyz - u.cameraPosition.xyz;
                key = -dot(d, d);
            } else if (mode == 2u) {
                key = p.posAge.w;
            } else {
                key = -p.posAge.w;
            }
        }
        keys.values[i] = vec2(key, float(i));
    }
    memoryBarrierBuffer();
    barrier();

    for (uint k = 2u; k <= size; k <<= 1u) {
        for (uint j = k >> 1u; j > 0u; j >>= 1u) {
            for (uint i = tid; i < size; i += threads) {
                uint partner = i ^ j;
                if (partner > i) {
                    bool ascending = (i & k) == 0u;
                    vec2 a = keys.values[i];
                    vec2 b = keys.values[partner];
                    if ((a.x > b.x) == ascending) {
                        keys.values[i] = b;
                        keys.values[partner] = a;
                    }
                }
            }
            memoryBarrierBuffer();
            barrier();
        }
    }

    for (uint i = tid; i < count; i += threads) {
        order.values[i] = uint(keys.values[i].y);
    }
}
)";
}
