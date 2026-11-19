// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
// Integrates the tiles of a probe volume bake's atlases (ProbeVolumeBaker; the layout is
// common-probe-bake.slang's) into a float target read back on the CPU: probesPerRow probes along
// a row of the target, texelsPerProbe texels each, drawn over the whole target in one quad.
//
// sh: projects the radiance of a probe's faces onto L1 spherical harmonics, three texels a probe,
// the L0 and the three L1 coefficients of the red, green and blue channels.
// validity: integrates the geometry into one texel a probe: the fraction of the sphere covered by
// back faces, and the vector to the closest back face.
// visibility: filters the distances into an octahedral map of the probe (Cigolle et al., JCGT
// 2014), a texel for each of its directions: the mean and the mean squared distance to the closest
// surface around the direction, weighted by a cosine power lobe (DDGI).
// @variant sh: VT_PROJECT_MODE=0
// @variant validity: VT_PROJECT_MODE=1
// @variant visibility: VT_PROJECT_MODE=2
import vtquad;
#include "bindings.slang"

struct ProbeProjectUniforms
{
    float4 atlas;    // face size, probes along a row of the atlas
    float4 layout;   // texels a probe, probes along a row of the target, probe count
    float4 visibility;   // texels along a side of a map, the distance the distances are clamped to
};
VT_DECLARE_QUAD_UNIFORMS(ProbeProjectUniforms, u)

// The radiance of the tiles, or the albedo and the hit flag of the geometry.
VT_DECLARE_QUAD_TEXEL_INPUT0(probeAtlas0)
// The back face flag and the distance of the geometry.
VT_DECLARE_QUAD_TEXEL_INPUT1(probeAtlas1)

float3 faceDirection(int face, float u, float v)
{
    if (face == 0) return float3(-1.0, v, -u);
    if (face == 1) return float3(1.0, v, u);
    if (face == 2) return float3(-u, 1.0, -v);
    if (face == 3) return float3(-u, -1.0, v);
    if (face == 4) return float3(-u, v, 1.0);
    return float3(u, v, -1.0);
}

// The direction of a point of the octahedral map, for coordinates in -1..1.
float3 octahedralDirection(float2 p)
{
    float3 n = float3(p, 1.0 - abs(p.x) - abs(p.y));
    const float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

[shader("vertex")]
QuadVarying probeProjectVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 probeProjectFragment(QuadVarying input) : SV_Target0
{
    const int2 fragment = int2(floor(input.position.xy));
    const int texelsPerProbe = int(u.layout.x);
    const int probe = fragment.y * int(u.layout.y) + fragment.x / texelsPerProbe;
#if VT_PROJECT_MODE != 1
    const int localTexel = fragment.x % texelsPerProbe;
#endif
    if (probe >= int(u.layout.z)) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const int size = int(u.atlas.x);
    const int perRow = int(u.atlas.y);
    const float texelSize = 2.0 / u.atlas.x;
    const int2 origin = int2((probe % perRow) * size * 3, (probe / perRow) * size * 2);

    float4 result = float4(0.0, 0.0, 0.0, 0.0);
    float weightSum = 0.0;
    float closest = 1e10;

#if VT_PROJECT_MODE == 2
    // The direction of the texel of the octahedral map.
    const int mapSize = int(u.visibility.x);
    const float2 mapCoords = float2(float(localTexel % mapSize), float(localTexel / mapSize));
    const float3 mapDir = octahedralDirection((mapCoords + 0.5) / u.visibility.x * 2.0 - 1.0);
    float lobeSum = 0.0;
#endif

    for (int face = 0; face < 6; face++) {
#if VT_PROJECT_MODE == 2
        // The lobe is negligible beyond 30 degrees, so the faces whose texels are all further
        // from the direction (beyond the 55 degrees of the corners of a face) are skipped.
        if (dot(mapDir, faceDirection(face, 0.0, 0.0)) < 0.09) {
            continue;
        }
#endif
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                // y counts from the face's top row, which sees v = +1.
                const float uf = (float(x) + 0.5) * texelSize - 1.0;
                const float vf = 1.0 - (float(y) + 0.5) * texelSize;

                // The solid angle of the texel, up to a constant factor.
                const float t = 1.0 + uf * uf + vf * vf;
                const float weight = 1.0 / (t * sqrt(t));
                weightSum += weight;

                const float3 dir = normalize(faceDirection(face, uf, vf));
                const int3 texel = int3(origin + int2((face % 3) * size + x, (face / 3) * size + y), 0);

#if VT_PROJECT_MODE == 1
                // r: 1 for a back face, g: the distance to it.
                const float4 geometry = probeAtlas1.Load(texel);
                result.x += geometry.r * weight;
                if (geometry.r > 0.5 && geometry.g < closest) {
                    closest = geometry.g;
                    result.yzw = dir * geometry.g;
                }
#elif VT_PROJECT_MODE == 2
                // The distance to the closest surface; the texels without a surface are far.
                const float hit = probeAtlas0.Load(texel).a;
                const float distance = probeAtlas1.Load(texel).g;
                const float maxDistance = u.visibility.y;
                const float dist = hit > 0.5 ? min(distance, maxDistance) : maxDistance;
                const float lobe = weight * pow(max(dot(mapDir, dir), 0.0), 50.0);
                result.xy += float2(dist, dist * dist) * lobe;
                lobeSum += lobe;
#else
                // The L0 and L1 basis functions.
                const float4 radiance = probeAtlas0.Load(texel);
                result += radiance[localTexel] * weight * float4(0.282095, 0.488603 * dir);
#endif
            }
        }
    }

#if VT_PROJECT_MODE == 1
    // The fraction of the sphere covered by back faces.
    result.x /= weightSum;
    return result;
#elif VT_PROJECT_MODE == 2
    return float4(result.xy / max(lobeSum, 1e-12), 0.0, 1.0);
#else
    // The weights add up to the solid angle of the sphere, 4 pi.
    return result * (12.566371 / weightSum);
#endif
}
