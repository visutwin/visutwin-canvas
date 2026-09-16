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

// ── Clustered atlas: omni faces ─────────────────────────────────────────────
// Upstream's getCubemapFaceCoordinates with its V term NEGATED: the dominant axis
// of the unnormalized light-to-fragment direction picks the face (+X, -X, +Y, -Y,
// +Z, -Z — the order LightCamera::pointLightRotations renders them), the other two
// axes map to a UV within it, and `tileOffset` is the face's column and row in the
// slot's 3x3 tile grid. The faces are rendered by upstream's own camera rotations,
// but into top-down storage where upstream's is bottom-up, so v runs the other way.
// Mirrored in LightTextureAtlas::cubemapFaceCoordinates, which a test holds against
// the face cameras' real projection — that test is what found the sign.
static inline float2 getCubemapFaceCoordinates(float3 dir, thread float2& tileOffset)
{
    const float3 vAbs = abs(dir);
    float ma;
    float2 uv;
    if (vAbs.z >= vAbs.x && vAbs.z >= vAbs.y) {          // +Z / -Z
        ma = 0.5 / vAbs.z;
        uv = float2(dir.z < 0.0 ? -dir.x : dir.x, dir.y);
        tileOffset = float2(2.0, dir.z < 0.0 ? 1.0 : 0.0);
    } else if (vAbs.y >= vAbs.x) {                        // +Y / -Y
        ma = 0.5 / vAbs.y;
        uv = float2(dir.x, dir.y < 0.0 ? dir.z : -dir.z);
        tileOffset = float2(1.0, dir.y < 0.0 ? 1.0 : 0.0);
    } else {                                              // +X / -X
        ma = 0.5 / vAbs.x;
        uv = float2(dir.x < 0.0 ? dir.z : -dir.z, dir.y);
        tileOffset = float2(0.0, dir.x < 0.0 ? 1.0 : 0.0);
    }
    return uv * ma + 0.5;
}

// Atlas UV of `dir` for an omni light whose slot is `rect` = (x, y, size, edge
// pixels), atlas resolution `resolution`. The face was rendered a few pixels wider
// than 90 degrees (the edge), so its 90-degree content sits inset by the same
// amount and the UV is inset to meet it — a filter kernel at the tile edge then
// stays inside the tile.
static inline float2 getCubemapAtlasCoordinates(float4 rect, float resolution, float3 dir)
{
    float2 tileOffset;
    float2 uv = getCubemapFaceCoordinates(dir, tileOffset);
    const float faceSize = rect.z / 3.0;
    const float tileSize = resolution * faceSize;
    const float offset = rect.w / max(tileSize, 1.0);
    uv = uv * (1.0 - 2.0 * offset) + offset;
    uv = uv * faceSize + tileOffset * faceSize + rect.xy;
    return uv;
}

// Visibility of a fragment at `lightToFrag` from a clustered omni light: the face
// stores perspective depth over [near, far], compared against the fragment's own
// dominant-axis distance with the same RELATIVE bias the cubemap path applies
// before the projection (see the omni block of forward-fragment-lights).
// `depthParams` = (near, far, relative bias, unused).
static inline float getShadowOmniClusteredPCF3(depth2d<float> shadowAtlas, float4 rect, float4 depthParams,
                                               float3 lightToFrag)
{
    const float resolution = float(shadowAtlas.get_width());
    const float2 uv = getCubemapAtlasCoordinates(rect, resolution, lightToFrag);
    const float3 absDir = abs(lightToFrag);
    const float d = max(absDir.x, max(absDir.y, absDir.z));
    const float nearVal = depthParams.x;
    const float farVal = depthParams.y;
    const float dBiased = d * (1.0 - depthParams.z);
    const float denom = (farVal - nearVal) * dBiased;
    const float compareValue = farVal * (dBiased - nearVal) / max(denom, 1e-6);
    return getShadowPCF3x3(shadowAtlas, uv, compareValue, resolution);
}
