// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.10.2026
//
// The clipping planes of the mesh instance (MeshInstance::setClippingPlanes) and of the camera
// (Camera::setClippingPlanes), both in the instance's copy of the material block. Each plane is
// in world space, (normal, distance); clipParams holds the number of each set's planes and 1
// where a set clips only what is behind all of its planes. Each set is tested on its own, and
// only in a variant that has it: a variant with neither has no discard (early depth testing).

// Whether the world position is behind the plane, on the side opposite to its normal.
bool isBehindClipPlane(float4 plane, float3 worldPos)
{
    return dot(plane.xyz, worldPos) + plane.w < 0.0;
}

// Discards the fragment at the world position when either set of planes clips it: when it is
// behind any plane of the set, or under intersection clipping only when it is behind all of them.
void applyClipPlanes(float3 worldPos)
{
    if (vtFeatureEnabled(VT_FEATURE_CLIP_PLANES_MESH_BIT)) {
        const int count = min(int(material.clipParams.x), 8);
        bool behindAny = false;
        bool behindAll = count > 0;
        for (int i = 0; i < count; i++) {
            const bool behind = isBehindClipPlane(material.clipPlanes[i], worldPos);
            behindAny = behindAny || behind;
            behindAll = behindAll && behind;
        }
        if (material.clipParams.y > 0.5 ? behindAll : behindAny) {
            discard;
        }
    }
    if (vtFeatureEnabled(VT_FEATURE_CLIP_PLANES_CAMERA_BIT)) {
        const int count = min(int(material.clipParams.z), 8);
        bool behindAny = false;
        bool behindAll = count > 0;
        for (int i = 0; i < count; i++) {
            const bool behind = isBehindClipPlane(material.cameraClipPlanes[i], worldPos);
            behindAny = behindAny || behind;
            behindAll = behindAll && behind;
        }
        if (material.clipParams.w > 0.5 ? behindAll : behindAny) {
            discard;
        }
    }
}

// The visibility range's dithered crossfade (MeshInstance::setVisibilityRange), in the same copy of
// the block. A fade f keeps the fragments whose noise is below f (fading out), -f those at or above
// 1 - f (fading in), so a level fading out and the next fading in over one band split every pixel
// between them. The noise is the blue-noise texture of the opacity dither, read LINEARLY (that
// dither's gamma would skew the fraction kept) and offset by lodFadeParams.yz, the camera's TAA
// dither jitter (0 for a shadow, and for a camera that does not jitter): both levels read the same
// offset, so they stay complementary in every frame, and a new pattern each frame lets TAA blend
// the two instead of showing the dither.
float lodFadeNoise(float2 screenPos)
{
    const float2 uv = frac(screenPos / 32.0 + material.lodFadeParams.yz);
    const uint2 texel = min(uint2(uv * 32.0), uint2(31u));
    const uint index = texel.y * 32u + texel.x;
    return float((kBlueNoise32[index >> 2u] >> ((index & 3u) * 8u)) & 0xFFu) / 255.0;
}

// The range's fade at a distance: the shader twin of VisibilityRange::fadeAt (visibilityRange.h),
// over lodRangeParams (fade in from, drawn whole from, fade out from, hidden from). Change both.
float lodFadeAtDistance(float distance)
{
    const float4 r = material.lodRangeParams;
    if (distance >= r.w) {
        return 0.0;
    }
    if (distance >= r.z) {
        return (r.w - distance) / (r.w - r.z);
    }
    if (distance < r.x) {
        return 0.0;
    }
    if (distance < r.y) {
        const float f = (distance - r.x) / (r.y - r.x);
        return f > 0.0 ? -f : 0.0;
    }
    return 1.0;
}

void applyLodFadeValue(float2 screenPos, float fade)
{
    if (fade >= 1.0) {
        return;
    }
    const float noise = lodFadeNoise(screenPos);
    if (fade == 0.0 || (fade > 0.0 ? noise >= fade : noise < 1.0 + fade)) {
        discard;
    }
}

// `instanceFade`: the fade the instanced vertex stage computed for this instance (the
// ForwardVarying's lodFade), read under VT_FEATURE_LOD_FADE_INSTANCED; the whole instance's
// fade comes from the block under VT_FEATURE_LOD_FADE. Neither variant: no discard at all.
void applyLodFade(float2 screenPos, float instanceFade)
{
    if (vtFeatureEnabled(VT_FEATURE_LOD_FADE_BIT)) {
        applyLodFadeValue(screenPos, material.lodFadeParams.x);
    }
    if (vtFeatureEnabled(VT_FEATURE_LOD_FADE_INSTANCED_BIT)) {
        applyLodFadeValue(screenPos, instanceFade);
    }
}
