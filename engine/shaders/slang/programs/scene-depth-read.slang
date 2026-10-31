// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Reads a camera's scene depth over a grid of samples (SceneDepthReader): sample (i, j) of a
// width x height grid sits at the centre of its cell of a region of the camera's viewport, given
// in normalized coordinates from the viewport's bottom-left corner, and row j = 0 is the region's
// bottom. Each sample is the linear camera depth in world units, decoded by `vtscenemaps` from
// whatever the producer stored, and infinity where nothing was rendered (at the far clip).
import vtscenemaps;

struct DepthReadParams
{
    float4 rect;    // x, y: the region's bottom-left corner, z, w: its size (normalized)
    uint4 grid;     // x, y: the samples across and up
};

[[vk::binding(0, 0)]] [vk::image_format("r32f")] RWTexture2D<float> samples : register(u0);
[[vk::binding(1, 0)]] ConstantBuffer<DepthReadParams> params : register(b0);

[shader("compute")]
[numthreads(8, 8, 1)]
void sceneDepthReadMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= params.grid.x || id.y >= params.grid.y) {
        return;
    }
    // the sample's place in the viewport, v running up from the bottom
    const float u = params.rect.x + (float(id.x) + 0.5) / float(params.grid.x) * params.rect.z;
    const float v = params.rect.y + (float(id.y) + 0.5) / float(params.grid.y) * params.rect.w;
    const float4 viewport = float4(sceneDepthViewport());
    const int2 texel = int2(viewport.xy + float2(u, 1.0 - v) * viewport.zw);
    const uint2 size = sceneDepthSize();
    const int2 clamped = clamp(texel, int2(0, 0), int2(size) - int2(1, 1));

    const float depth = sceneDepthLinear(clamped);
    // at the far clip nothing was rendered: no surface, so an infinite distance
    const float far = sceneDepthFarClip();
    const bool empty = depth >= far * (1.0 - 1e-5);
    samples[id.xy] = empty ? asfloat(0x7f800000u) : depth;
}
