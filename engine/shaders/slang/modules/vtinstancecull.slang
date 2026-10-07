// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// What the two GPU instance-cull kernels share: the bindings, in Compute's name-order
// contract (buffers "args", "input", "output" at 0, 1, 2 and the 128-byte uniform block
// after them, buffer(3) on Metal and binding 3 on Vulkan), the 80-byte instance record
// (model matrix + colour) and the 20-byte indexed-indirect arguments both APIs share
// (index count, instance count, first index, vertex offset, first instance).
module vtinstancecull;

public static const uint kCullThreads = 64u;

public struct CullParams
{
    public float4 frustumPlanes[6];   // (nx, ny, nz, d); dot(n, p) + d >= 0 is inside
    public float boundingSphereRadius;
    public uint instanceCount;
    public uint indexCount;
    public uint indexStart;
    public int baseVertex;
    public uint baseInstance;
    public float pad0;
    public float pad1;
};

/// The model matrix as its four COLUMNS, so the translation is columns[3] with no
/// question of how a matrix type is indexed.
public struct InstanceData
{
    public float4 columns[4];
    public float4 diffuseColor;
};

// The draw arguments as five uints: [1] is the instance count the cull appends to.
[[vk::binding(0, 0)]] public RWStructuredBuffer<uint> args : register(u0);
[[vk::binding(1, 0)]] public StructuredBuffer<InstanceData> input : register(t1);
[[vk::binding(2, 0)]] public RWStructuredBuffer<InstanceData> output : register(u2);
[[vk::binding(3, 0)]] public ConstantBuffer<CullParams> params : register(b3);
