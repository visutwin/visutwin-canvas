// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The direction maps and the RGBP packing the environment bakes share.
module vtenv;

public static const float ENV_PI = 3.141592653589793;
/// TextureProjection's octahedral id (platform/graphics/constants.h).
public static const uint PROJ_OCTAHEDRAL = 3u;

public float3 uvToDirEquirect(float2 uv)
{
    const float phi   = (uv.x * 2.0 - 1.0) * ENV_PI;
    const float theta = (1.0 - uv.y) * ENV_PI - ENV_PI * 0.5;
    const float c = cos(theta);
    return float3(sin(phi) * c, sin(theta), cos(phi) * c);
}

/// atan2(0, 0) is undefined: azimuth 0 at the poles, as the shading lookups pick it.
public float2 dirToUvEquirect(float3 dir)
{
    const float phi   = (dir.x == 0.0 && dir.z == 0.0) ? 0.0 : atan2(dir.x, dir.z);
    const float theta = asin(clamp(dir.y, -1.0, 1.0));
    return float2(phi / (2.0 * ENV_PI) + 0.5, 1.0 - (theta / ENV_PI + 0.5));
}

public float3 uvToDirOctahedral(float2 uv)
{
    const float2 f = uv * 2.0 - 1.0;
    float3 n = float3(f.x, 1.0 - abs(f.x) - abs(f.y), f.y);
    const float t = max(-n.y, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.z += (n.z >= 0.0) ? -t : t;
    return normalize(n);
}

public float2 dirToUvOctahedral(float3 dir)
{
    const float3 a = abs(dir);
    const float invL1 = 1.0 / max(a.x + a.y + a.z, 1e-8);
    float2 t = float2(dir.x, dir.z) * invL1;
    if (dir.y < 0.0) {
        t = (1.0 - abs(float2(t.y, t.x))) * float2(t.x >= 0.0 ? 1.0 : -1.0, t.y >= 0.0 ? 1.0 : -1.0);
    }
    return t * 0.5 + 0.5;
}

public float4 packRgbp(float3 color)
{
    const float3 lin = max(color, float3(0.0, 0.0, 0.0));
    const float3 s = float3(sqrt(lin.r), sqrt(lin.g), sqrt(lin.b));
    const float maxVal = max(max(s.r, s.g), max(s.b, 1.0 / 255.0));
    const float a = clamp((8.0 - maxVal) / 7.0, 0.0, 1.0);
    const float scale = -a * 7.0 + 8.0;
    return float4(clamp(s / scale, 0.0, 1.0), a);
}

public float3 decodeIfSrgb(float3 color, bool decode)
{
    return decode ? pow(max(color, float3(0.0, 0.0, 0.0)), float3(2.2, 2.2, 2.2)) : color;
}
