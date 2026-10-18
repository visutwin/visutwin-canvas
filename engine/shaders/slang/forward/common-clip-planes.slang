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
