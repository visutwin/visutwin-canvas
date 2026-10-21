// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.10.2026
//
// A camera's scene depth and colour maps in a compute kernel, attached to the Compute with
// setSceneDepthMap / setSceneColorMap (`import vtscenemaps;`). The Compute fills these
// resources itself at every dispatch, at FIXED slots outside its name-ordered table (the
// constants beside ComputeSceneMapsBlock in compute.h; change both): Metal texture 30 / 31,
// sampler 15 and buffer 30, Vulkan set 1 bindings 0-3.
//
// Depth:
//   sceneDepthSize() -> uint2                  the size of the depth map
//   sceneDepthViewport() -> uint4              the viewport the camera rendered it with, in
//                                              texels (row 0 at the top): x, y, width, height
//   sceneDepthNearClip(), sceneDepthFarClip()  the clip planes it was rendered with
//   sceneDepthLinear(int2 texel) -> float      the linear camera depth, in world units
//   sceneDepthWorldPosition(int2 texel)        the world position of the surface at the texel,
//                                              which needs to be inside the viewport
// Colour (as the camera stored it: linear under a camera frame, gamma-encoded otherwise):
//   sceneColorSize(int lod) -> uint2
//   sceneColorLoad(int2 texel, int lod) -> float4
//   sceneColorSample(float2 uv, float lod) -> float4    filtered, clamped at the edges
//   sceneColorToLinear(float3), sceneColorToDisplay(float3)
//
// DEVIATION: one module for both maps where upstream has a chunk each, and the storage
// (linear, reciprocal, gamma) is read from the block's flags rather than compiled into a
// variant; every producer in this engine stores depth buffer values. A float map is always
// sampled filtered (both backends filter 32-bit float).
module vtscenemaps;

public struct SceneMapsData
{
    // 1 / far clip, far clip, near clip, 1 for an orthographic projection or 0
    public float4 depthCameraParams;
    // the inverse view-projection the depth was rendered with (GL clip: NDC z in [-1, 1])
    public float4x4 depthViewProjectionInverse;
    public uint4 depthViewport;
    // x: the depth is linear, y: the reciprocal of the linear depth, z: the colour is gamma-encoded
    public uint4 flags;
};

[[vk::binding(0, 1)]] public Texture2D<float> computeSceneDepthMap : register(t30);
[[vk::binding(1, 1)]] public Texture2D<float4> computeSceneColorMap : register(t31);
[[vk::binding(2, 1)]] public SamplerState computeSceneColorSampler : register(s15);
[[vk::binding(3, 1)]] public ConstantBuffer<SceneMapsData> computeSceneMaps : register(b30);

public uint2 sceneDepthSize()
{
    uint width, height, levels;
    computeSceneDepthMap.GetDimensions(0, width, height, levels);
    return uint2(width, height);
}

public uint4 sceneDepthViewport()
{
    return computeSceneMaps.depthViewport;
}

public float sceneDepthNearClip()
{
    return computeSceneMaps.depthCameraParams.z;
}

public float sceneDepthFarClip()
{
    return computeSceneMaps.depthCameraParams.y;
}

public float sceneDepthLinear(int2 texel)
{
    const float value = computeSceneDepthMap.Load(int3(texel, 0)).r;
    const float4 params = computeSceneMaps.depthCameraParams;
    if (computeSceneMaps.flags.x != 0u) {
        if (computeSceneMaps.flags.y != 0u) {
            // a coverage-weighted average of the reciprocal depths; zero is a texel nothing
            // was rendered to, which reads as the far clip
            return value > 0.0 ? 1.0 / value : params.y;
        }
        return value;
    }
    // the depth buffer's value in [0, 1], linearised for either projection
    const float perspective = (params.z * params.y) / (params.y + value * (params.z - params.y));
    const float orthographic = params.z + value * (params.y - params.z);
    return params.w > 0.5 ? orthographic : perspective;
}

float3 sceneDepthUnproject(float3 ndc)
{
    const float4 position = mul(float4(ndc, 1.0), computeSceneMaps.depthViewProjectionInverse);
    return position.xyz / position.w;
}

public float3 sceneDepthWorldPosition(int2 texel)
{
    // the position of the texel within the viewport the camera rendered to; NDC +y is the
    // top row
    const float4 viewport = float4(computeSceneMaps.depthViewport);
    const float2 uv = (float2(texel) + 0.5 - viewport.xy) / viewport.zw;
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    if (computeSceneMaps.flags.x != 0u) {
        // the linear depth places the surface between the texel's points on the near and the
        // far plane, along which the depth changes linearly
        const float4 params = computeSceneMaps.depthCameraParams;
        const float3 nearPoint = sceneDepthUnproject(float3(ndc, -1.0));
        const float3 farPoint = sceneDepthUnproject(float3(ndc, 1.0));
        return lerp(nearPoint, farPoint, (sceneDepthLinear(texel) - params.z) / (params.y - params.z));
    }
    // the depth buffer's value is where the surface was projected to, remapped from [0, 1]
    // to the GL clip range the matrix expects
    const float depth = computeSceneDepthMap.Load(int3(texel, 0)).r;
    return sceneDepthUnproject(float3(ndc, depth * 2.0 - 1.0));
}

public uint2 sceneColorSize(int lod)
{
    uint width, height, levels;
    computeSceneColorMap.GetDimensions(uint(lod), width, height, levels);
    return uint2(width, height);
}

public float4 sceneColorLoad(int2 texel, int lod)
{
    return computeSceneColorMap.Load(int3(texel, lod));
}

public float4 sceneColorSample(float2 uv, float lod)
{
    return computeSceneColorMap.SampleLevel(computeSceneColorSampler, uv, lod);
}

public float3 sceneColorToLinear(float3 color)
{
    return computeSceneMaps.flags.z != 0u ? pow(max(color, float3(0.0)), float3(2.2)) : color;
}

public float3 sceneColorToDisplay(float3 color)
{
    return computeSceneMaps.flags.z != 0u ? color : pow(max(color, float3(0.0)), float3(1.0 / 2.2));
}
