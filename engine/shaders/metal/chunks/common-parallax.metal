// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
static inline float2 parallaxOcclusionMap(float2 uv, float3 viewDirTS,
                                           texture2d<float> heightMap, sampler s,
                                           float heightScale) {
    // Adaptive step count: more steps at grazing angles where parallax is most visible.
    const int minSteps = 8;
    const int maxSteps = 32;
    const int numSteps = int(mix(float(maxSteps), float(minSteps), abs(viewDirTS.z)));
    const float layerDepth = 1.0 / float(numSteps);

    // UV step per layer along view direction projected onto surface.
    const float2 deltaUV = viewDirTS.xy * heightScale / (abs(viewDirTS.z) + 1e-5) / float(numSteps);

    float2 curUV = uv;
    float curLayerDepth = 0.0;
    float curHeight = 1.0 - heightMap.sample(s, curUV).r;

    // Step through layers until we go below the surface.
    for (int i = 0; i < maxSteps; ++i) {
        if (curLayerDepth >= curHeight) break;
        curUV -= deltaUV;
        curHeight = 1.0 - heightMap.sample(s, curUV).r;
        curLayerDepth += layerDepth;
    }

    // Linear interpolation between last two layers for smooth result.
    const float2 prevUV = curUV + deltaUV;
    const float afterDepth = curHeight - curLayerDepth;
    const float beforeDepth = (1.0 - heightMap.sample(s, prevUV).r) - (curLayerDepth - layerDepth);
    const float weight = afterDepth / (afterDepth - beforeDepth + 1e-6);
    return mix(curUV, prevUV, weight);
}
