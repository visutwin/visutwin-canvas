// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
// What a probe volume bake (ProbeVolumeBaker) needs of the forward program.
//
// The probes capture into atlases of tiles: each probe a block of 3 x 2 square faces of
// bakeAtlas.x texels, in rows of bakeAtlas.y probes, the first row at the top. The texel (x, y) of
// a face, y counted from the face's top row, sees along probeFaceDirection(face, u, v), u and v its
// centre in -1..1 with v up (the camera of the face renders NDC +y into the top row). The same
// layout is the projection's (programs/probe-volume-project.slang).
//
// Under VT_FEATURE_PROBE_GBUFFER (a frame switch, set while a bake's cameras capture the geometry)
// a surface writes what it is instead of its colour: the diffuse albedo and a hit flag, the back
// face flag, the distance to the probe and the octahedral normal, and the emission.
//
// Under VT_FEATURE_PROBE_LIGHTING (the material of the bake's lighting quad, drawn over the whole
// radiance atlas) a fragment takes the surface its texel of the atlas saw: probeBakeSurface.

// The direction through the point (u, v) of a face; the faces' cameras are turned as
// ProbeVolumeBaker's face rotations turn them.
float3 probeFaceDirection(int face, float u, float v)
{
    if (face == 0) return float3(-1.0, v, -u);
    if (face == 1) return float3(1.0, v, u);
    if (face == 2) return float3(-u, 1.0, -v);
    if (face == 3) return float3(-u, -1.0, v);
    if (face == 4) return float3(-u, v, 1.0);
    return float3(u, v, -1.0);
}

// The face and the point (u, v) a direction passes through, the inverse of probeFaceDirection.
float3 probeDirectionFace(float3 dir)
{
    const float3 a = abs(dir);
    if (a.x >= a.y && a.x >= a.z) {
        return dir.x < 0.0 ? float3(0.0, -dir.z / a.x, dir.y / a.x) : float3(1.0, dir.z / a.x, dir.y / a.x);
    }
    if (a.y >= a.z) {
        return dir.y > 0.0 ? float3(2.0, -dir.x / a.y, -dir.z / a.y) : float3(3.0, -dir.x / a.y, dir.z / a.y);
    }
    return dir.z > 0.0 ? float3(4.0, -dir.x / a.z, dir.y / a.z) : float3(5.0, dir.x / a.z, dir.y / a.z);
}

// The octahedral encoding of a unit vector, in -1..1 (Cigolle et al., JCGT 2014).
float2 probeOctahedralEncode(float3 n)
{
    const float3 p = n / (abs(n.x) + abs(n.y) + abs(n.z));
    return p.z >= 0.0 ? p.xy : (1.0 - abs(p.yx)) * float2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
}

float3 probeOctahedralDecode(float2 e)
{
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    const float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

// The surface a texel of the radiance atlas saw, as the lit path takes it.
struct ProbeBakeSurface
{
    float3 position;
    float3 normal;
    float3 albedo;
    float3 emission;
    float3 viewDir;
    float distance;
};

ProbeBakeSurface probeBakeSurface(float2 fragCoord)
{
    const ProbeVolumeData pv = lighting.probeVolume;

    // The probe, face and texel of the fragment.
    const int size = int(pv.bakeAtlas.x);
    const int2 texel = int2(fragCoord);
    const int tileX = texel.x / (size * 3);
    const int tileY = texel.y / (size * 2);
    const int probe = tileY * int(pv.bakeAtlas.y) + tileX;
    const int localX = texel.x - tileX * size * 3;
    const int localY = texel.y - tileY * size * 2;
    const int face = (localY / size) * 3 + localX / size;
    const float u = (float(localX % size) + 0.5) / pv.bakeAtlas.x * 2.0 - 1.0;
    const float v = 1.0 - (float(localY % size) + 0.5) / pv.bakeAtlas.x * 2.0;
    const float3 dir = normalize(probeFaceDirection(face, u, v));

    const int positionsWidth = int(pv.bakeAtlas.w);
    const float3 probePosition =
        VT_BINDLESS_TEXTURE(pv.bakeLanes.w).Load(int3(probe % positionsWidth, probe / positionsWidth, 0)).xyz;
    const float4 geometry0 = VT_BINDLESS_TEXTURE(pv.bakeLanes.x).Load(int3(texel, 0));
    const float4 geometry1 = VT_BINDLESS_TEXTURE(pv.bakeLanes.y).Load(int3(texel, 0));

    ProbeBakeSurface surface;
    surface.albedo = float3(0.0);
    surface.emission = float3(0.0);
    surface.distance = geometry1.g;
    surface.viewDir = -dir;
    if (geometry0.a < 0.5) {
        // The sky, far along the direction, from the sky atlas captured at the volume's centre.
        const float3 faceUv = probeDirectionFace(dir);
        const float skySize = pv.bakeAtlas.z;
        const int skyFace = int(faceUv.x);
        const float2 origin = float2(float(skyFace % 3), float(skyFace / 3)) * skySize;
        const float2 local = clamp(float2(faceUv.y + 1.0, 1.0 - faceUv.z) * 0.5 * skySize, 0.5, skySize - 0.5);
        const float2 uv = (origin + local) / float2(skySize * 3.0, skySize * 2.0);
        const uint skyLane = pv.bakeSkyLane.x;
        surface.emission = VT_BINDLESS_TEXTURE(skyLane).SampleLevel(VT_BINDLESS_SAMPLER(skyLane), uv, 0.0).rgb;
        surface.distance = 1000.0;
        surface.normal = -dir;
    } else if (geometry1.r < 0.5) {
        surface.albedo = geometry0.rgb;
        surface.normal = probeOctahedralDecode(geometry1.ba);
        if (pv.bakeLanes.z != 0u) {
            surface.emission = VT_BINDLESS_TEXTURE(pv.bakeLanes.z).Load(int3(texel, 0)).rgb;
        }
    } else {
        // A back face, black.
        surface.normal = -dir;
    }
    surface.position = probePosition + dir * surface.distance;
    return surface;
}
