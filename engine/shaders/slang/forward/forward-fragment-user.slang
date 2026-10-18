// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 17.10.2026
//
// Hooks of the lit fragment stage an application replaces with a chunk override of this file
// (Material::setShaderChunk("forward-fragment-user", source)), without copying the tail. Both
// are the identity here, which the compiler removes, so a material without an override renders
// exactly as before. The override defines both functions, and may declare what they read above
// them: its scene-wide values are lighting.userData[0..3] and its texture VT_DECLARE_userMap
// (Scene::setUserShaderData), sampled through materialSampler.

// The fog factor (1 unfogged, 0 the fog colour) of a fragment at `worldPos`, after the fog
// curve and before the fog is mixed in.
float forwardUserFogFactor(float fogFactor, float3 worldPos)
{
    return fogFactor;
}

// The colour the lit fragment writes, as it is written: linear HDR on a camera frame's scene,
// otherwise already exposed, tone mapped and gamma-encoded. `worldNormal` is the shading
// normal (normal maps applied).
float3 forwardUserFragmentEnd(float3 color, float3 worldPos, float3 worldNormal)
{
    return color;
}
