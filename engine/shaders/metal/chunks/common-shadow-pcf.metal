// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
static inline float getShadowPCF3x3(depth2d<float> shadowMap, float2 shadowUv, float depth, float resolution) {
    constexpr sampler shadowCompSampler(coord::normalized, filter::linear,
                                        compare_func::less_equal, address::clamp_to_edge);

    const float z = depth;
    const float2 uv = shadowUv * resolution;         // UV → texel space
    const float shadowMapSizeInv = 1.0 / resolution;
    const float2 base_uv_full = floor(uv + 0.5);
    const float s = (uv.x + 0.5 - base_uv_full.x);
    const float t = (uv.y + 0.5 - base_uv_full.y);
    const float2 base_uv = (base_uv_full - float2(0.5)) * shadowMapSizeInv;

    const float uw0 = (3.0 - 2.0 * s);
    const float uw1 = (1.0 + 2.0 * s);

    const float u0 = ((2.0 - s) / uw0 - 1.0) * shadowMapSizeInv + base_uv.x;
    const float u1 = (s / uw1 + 1.0) * shadowMapSizeInv + base_uv.x;

    const float vw0 = (3.0 - 2.0 * t);
    const float vw1 = (1.0 + 2.0 * t);

    const float v0 = ((2.0 - t) / vw0 - 1.0) * shadowMapSizeInv + base_uv.y;
    const float v1 = (t / vw1 + 1.0) * shadowMapSizeInv + base_uv.y;

    float sum = 0.0;
    sum += uw0 * vw0 * shadowMap.sample_compare(shadowCompSampler, float2(u0, v0), z);
    sum += uw1 * vw0 * shadowMap.sample_compare(shadowCompSampler, float2(u1, v0), z);
    sum += uw0 * vw1 * shadowMap.sample_compare(shadowCompSampler, float2(u0, v1), z);
    sum += uw1 * vw1 * shadowMap.sample_compare(shadowCompSampler, float2(u1, v1), z);

    return sum * (1.0 / 16.0);
}
