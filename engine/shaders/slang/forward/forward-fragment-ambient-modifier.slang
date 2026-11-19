// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
// The ambient modifier: forwardModifyAmbient receives the ambient diffuse irradiance of the scene
// at a surface (the SH probes, the environment atlas or the flat ambient, with the environment's
// intensity) and returns the surface's, and sets the scale of the surface's environment
// reflections (1: unchanged). The ambient chunk calls it along the normal, and along the flipped
// normal for diffuse transmission. A chunk override may replace it; it must define
// forwardModifyAmbient with this signature.
//
// DEVIATION: upstream's default modifier is the identity and its probe volume replaces the chunk;
// here the probe volume is the default, compiled in only under VT_FEATURE_PROBE_VOLUME, so its
// materials keep the bundled program.
//
// The probe volume (ProbeVolume, extras/probeVolume): a box grid of L1 SH probes of the incoming
// radiance. A 3D texture stores them as three sub-volumes stacked along its depth, for the red,
// green and blue channels, each texel holding the L0 and the three L1 coefficients of one channel
// of one probe, at the centres of the cells; the hardware filters them trilinearly between the
// eight probes around a position.
//
// To reduce light leaking, the weights of the eight probes can exclude the invalid probes, which
// are inside geometry, and favour the probes in front of the surface. The weights are collapsed
// into a single sample position the hardware filtering approximates them at, so this costs a
// fetch of the validity of the probes and no further filtered fetches. The probes moved out of
// geometry when baked (ProbeVolume::offsets) are interpolated at their grid positions, while the
// leak reduction and the visibility use the positions they were captured at.
//
// With visibility, the weights also exclude the probes the surface is hidden from: each probe has
// an octahedral map of the mean and the mean squared distance to the closest surface around each
// direction, in a tile of a 2D atlas, and its weight is the Chebyshev bound of the probability
// that it sees the surface (Majercik et al., DDGI): a filtered fetch per probe.
//
// The probes also occlude the environment reflections where they are darker than the
// environment, such as inside a closed room: the reflections scale by the ratio of the luminance
// of the probes' ambient to the environment's, up to 1.
//
// Under VT_FEATURE_PROBE_VOLUME_DEBUG the surface shows the probe of its cell, as it is stored,
// for small spheres drawn around the probes.

// Reads a probe volume texture through its bindless lane, at an explicit LOD.
float4 probeVolumeSample(uint lane, float3 uvw)
{
    return VT_BINDLESS_VOLUME(lane).SampleLevel(VT_BINDLESS_SAMPLER(lane), uvw, 0.0);
}

// The L0 and L1 coefficients of a channel. Half float texels store them as they are, 8-bit texels
// sqrt(L0 / scale) and L1 / (sqrt(3) L0) mapped to 0..1.
float4 decodeProbeVolumeTexel(float4 texel)
{
    const ProbeVolumeData pv = lighting.probeVolume;
    float l0 = texel.x * lerp(1.0, texel.x * pv.encoding.y, pv.encoding.x);
    float3 l1 = lerp(texel.yzw, (texel.yzw * 2.0 - 1.0) * (1.7320508 * l0), pv.encoding.x);
    return float4(l0, l1);
}

// The irradiance divided by pi of a channel, from the radiance SH of the channel.
float evaluateProbeVolumeChannel(float4 sh, float3 normal, float3 axis, float zonal)
{
    // L0 and L1 convolved with the clamped cosine lobe, band factors 1 and 2/3.
    float irradiance = 0.282095 * sh.x + 0.325735 * dot(sh.yzw, normal);

    // A quadratic zonal harmonic along the shared axis, its coefficient estimated from how
    // directional the L1 coefficients are (ZH3, Roughton et al., I3D 2024).
    float ratio = min(abs(dot(sh.yzw, axis)) / max(sh.x, 1e-6), 1.7320508);
    return irradiance + zonal * sh.x * (0.08 * ratio + 0.6 * ratio * ratio);
}

// The octahedral coordinates of a direction, in 0..1 (Cigolle et al., JCGT 2014).
float2 probeVolumeOctahedral(float3 dir)
{
    float2 p = dir.xy / (abs(dir.x) + abs(dir.y) + abs(dir.z));
    if (dir.z < 0.0) {
        p = (1.0 - abs(p.yx)) * float2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    }
    return p * 0.5 + 0.5;
}

// The visibility of a probe from a position: the Chebyshev bound of the probability that the
// probe sees the position, from the mean and the variance of the distances it sees that way.
float getProbeVolumeVisibility(float probe, float3 fromProbe)
{
    const ProbeVolumeData pv = lighting.probeVolume;

    // The direction in the map of the probe, inside the border of its tile.
    float tilesPerRow = pv.visibility.y;
    float size = pv.visibility.z;
    float row = floor((probe + 0.5) / tilesPerRow);
    float2 tile = float2(probe - row * tilesPerRow, row);
    float dist = length(fromProbe);
    float2 octahedral = probeVolumeOctahedral(fromProbe / max(dist, 1e-6));
    float2 uv = (tile * (size + 2.0) + 1.0 + octahedral * size) * pv.visibilityInvSize.xy;
    const uint lane = pv.lanes.z;
    float2 moments = VT_BINDLESS_TEXTURE(lane).SampleLevel(VT_BINDLESS_SAMPLER(lane), uv, 0.0).rg;

    if (dist <= moments.x) {
        return 1.0;
    }
    float variance = abs(moments.y - moments.x * moments.x);
    float delta = dist - moments.x;
    float chebyshev = variance / (variance + delta * delta);

    // Sharpened, and never zero, so that a position hidden from every probe keeps a fallback.
    return max(chebyshev * chebyshev * chebyshev, 0.05);
}

// The grid coordinates of the probes to sample at a surface, the probes at whole coordinates.
float3 getProbeVolumeGrid(float3 worldNormal, float3 worldPosition)
{
    const ProbeVolumeData pv = lighting.probeVolume;
    const float3 count = pv.count.xyz;

    // In front of the surface, so that the probes behind it contribute less.
    float3 position = worldPosition + worldNormal * pv.params.x;

    // Clamped to the probes, so that positions outside of them use the border probes.
    float3 grid = clamp((position - pv.boxMin.xyz) * pv.invSize.xyz * count - 0.5, float3(0.0), count - 1.0);

    // The weights of the eight probes around the position: trilinear, and to reduce leaking, zero
    // for the invalid probes and smaller for the probes behind the surface.
    float3 base = min(floor(grid), max(count - 2.0, float3(0.0)));
    float3 offset = grid - base;
    float4 baseTexel = probeVolumeSample(pv.lanes.y, (base + 0.5) / count);
    int validity = int(baseTexel.w * 255.0 + 0.5);
    float leakReduction = pv.params.w;
    bool useVisibility = pv.visibility.x > 0.0;
    bool useOffsets = leakReduction > 0.0 || useVisibility;

    float3 center = float3(0.0);
    float weightSum = 0.0;
    for (int i = 0; i < 8; i++) {
        float3 corner = float3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1));
        float3 trilinear = lerp(1.0 - offset, offset, corner);
        float weight = trilinear.x * trilinear.y * trilinear.z;

        // The position the probe was captured at, and the direction to it from the surface (DDGI).
        float3 probeGrid = base + corner;
        float3 probeOffset = float3(0.0);
        if (useOffsets) {
            float3 encoded = i == 0 ? baseTexel.xyz : probeVolumeSample(pv.lanes.y, (probeGrid + 0.5) / count).xyz;
            probeOffset = (encoded * 255.0 - 128.0) / 254.0;
        }
        float3 probePosition = pv.boxMin.xyz + (probeGrid + 0.5 + probeOffset) * pv.cellSize.xyz;
        float3 toProbe = probePosition - worldPosition;
        float facing = (dot(toProbe, worldNormal) * rsqrt(max(dot(toProbe, toProbe), 1e-8)) + 1.0) * 0.5;
        float valid = float((validity >> i) & 1);
        float probeWeight = lerp(1.0, valid * (facing * facing + 0.2), leakReduction);

        // The visibility of the probe from the position, with the small weights crushed, as a
        // little light is still visible due to the logarithmic perception (DDGI).
        if (useVisibility && weight > 0.0) {
            float probe = probeGrid.x + count.x * (probeGrid.y + count.y * probeGrid.z);
            probeWeight *= getProbeVolumeVisibility(probe, position - probePosition);
            if (probeWeight < 0.2) {
                probeWeight *= probeWeight * probeWeight * 25.0;
            }
        }

        weight *= probeWeight;
        center += corner * weight;
        weightSum += weight;
    }

    // The weighted centre of the probes, which the hardware filtering approximates the weights at.
    return weightSum > 1e-6 ? base + center / weightSum : grid;
}

float3 getProbeVolumeAmbient(float3 ambient, float3 worldNormal, float3 worldPosition, inout float reflectionOcclusion)
{
    const ProbeVolumeData pv = lighting.probeVolume;
    const float3 count = pv.count.xyz;

    float3 grid;
    if (vtFeatureEnabled(VT_FEATURE_PROBE_VOLUME_DEBUG_BIT)) {
        // The probe of the cell of the surface, as it is stored.
        grid = floor(clamp((worldPosition - pv.boxMin.xyz) * pv.invSize.xyz * count, float3(0.0), count - 1.0));
    } else {
        grid = getProbeVolumeGrid(worldNormal, worldPosition);
    }

    // The texture coordinates in the first of the three sub-volumes.
    float3 uvw = (grid + 0.5) / count;
    uvw.z /= 3.0;
    const uint lane = pv.lanes.x;
    float4 r = decodeProbeVolumeTexel(probeVolumeSample(lane, uvw));
    float4 g = decodeProbeVolumeTexel(probeVolumeSample(lane, uvw + float3(0.0, 0.0, 1.0 / 3.0)));
    float4 b = decodeProbeVolumeTexel(probeVolumeSample(lane, uvw + float3(0.0, 0.0, 2.0 / 3.0)));

    // The axis of the zonal harmonic, shared by the channels: the direction of the luminance L1.
    float3 axis = r.yzw * 0.2126 + g.yzw * 0.7152 + b.yzw * 0.0722;
    axis *= rsqrt(max(dot(axis, axis), 1e-12));
    float cosAxis = dot(worldNormal, axis);

    // The zonal basis function along the axis, convolved with the clamped cosine lobe (1/4).
    float zonal = pv.params.z * 0.25 * 0.315392 * (3.0 * cosAxis * cosAxis - 1.0);

    float3 irradiance = float3(
        evaluateProbeVolumeChannel(r, worldNormal, axis, zonal),
        evaluateProbeVolumeChannel(g, worldNormal, axis, zonal),
        evaluateProbeVolumeChannel(b, worldNormal, axis, zonal));
    float3 probeAmbient = max(irradiance, float3(0.0)) * pv.params.y;

    // Blended to the ambient of the environment with the distance outside of the volume.
    float3 outside = max(max(pv.boxMin.xyz - worldPosition, worldPosition - pv.boxMax.xyz), float3(0.0));
    float weight = clamp(1.0 - length(outside) * pv.blend.x, 0.0, 1.0) * pv.blend.y;

    // The environment reflections, occluded where the probes are darker than the environment.
    const float3 luminance = float3(0.2126, 0.7152, 0.0722);
    float occlusion = clamp(dot(probeAmbient, luminance) / max(dot(ambient, luminance), 1e-6), 0.0, 1.0);
    reflectionOcclusion = lerp(1.0, occlusion, weight * pv.blend.z);

    return lerp(ambient, probeAmbient, weight);
}

float3 forwardModifyAmbient(float3 ambient, float3 worldNormal, float3 worldPosition, inout float reflectionOcclusion)
{
    if (vtFeatureEnabled(VT_FEATURE_PROBE_VOLUME_BIT)) {
        return getProbeVolumeAmbient(ambient, worldNormal, worldPosition, reflectionOcclusion);
    }
    return ambient;
}
