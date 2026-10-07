// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// Particle sorting as one compute dispatch of ONE workgroup: each thread keys a share of
// the pool — sorted ascending: minus the squared distance to the camera (farthest drawn
// first), the life (newest first) or minus the life (oldest first) — then the workgroup
// runs a bitonic sort over the pool rounded up to a power of two (padding keys sort last),
// with a barrier between stages, and writes the particle indices in draw order. One
// workgroup keeps every stage's barrier a workgroup barrier, so the sort is a single
// dispatch at any pool size. Bindings in Compute's name order: buffers "order",
// "particles", "sortKeys" at 0, 1, 2 and the uniform block after them.

struct Particle
{
    float4 posAge;
    float4 velLifetime;
    float4 rotSeedSize;
    float4 motion;
};

struct SortUniforms
{
    float4 cameraPosition;   // xyz the camera, in the particles' space; w the sort mode
    float4 counts;           // x particle count, y the power of two sorted over
};

static const uint kThreads = 256u;

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> order : register(u0);
[[vk::binding(1, 0)]] StructuredBuffer<Particle> particles : register(t1);
[[vk::binding(2, 0)]] RWStructuredBuffer<float2> keys : register(u2);
[[vk::binding(3, 0)]] ConstantBuffer<SortUniforms> u : register(b3);

[shader("compute")]
[numthreads(256, 1, 1)]
void particleSortKernel(uint3 localId : SV_GroupThreadID)
{
    const uint tid = localId.x;
    const uint threads = kThreads;
    const uint count = uint(u.counts.x);
    const uint size = uint(u.counts.y);
    const uint mode = uint(u.cameraPosition.w + 0.5);
    for (uint i = tid; i < size; i += threads) {
        float key = asfloat(0x7F800000u);   // +infinity: padding sorts last
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
    DeviceMemoryBarrierWithGroupSync();
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
            DeviceMemoryBarrierWithGroupSync();
        }
    }
    for (uint i = tid; i < count; i += threads) {
        order[i] = uint(keys[i].y);
    }
}
