// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
    // Emissive.
    float3 emissive = material.emissiveColor.rgb;
    if (vtFeatureEnabled(VT_FEATURE_EMISSIVE_MAP_BIT)) {
        // The emissive TEXTURE is authored in sRGB and owes a decode, exactly like
        // the base-colour map. The FACTOR does not: StandardMaterial::updateUniforms
        // already pre-linearises setEmissive, which is why only this half is decoded.
        // Missing this made every emissive surface here about 2.3x too bright in
        // linear terms.
        // A map in a float format stores linear colours already (a render target a pass drew).
        const float3 emissiveSample = emissiveMap.Sample(uvEmissive).rgb;
        emissive *= vtFeatureEnabled(VT_FEATURE_EMISSIVE_MAP_LINEAR_BIT) ? emissiveSample : srgbToLinear(emissiveSample);
    }
    // `emissiveVertexColor` (bit 23): the vertex color tints emissive.
    if ((material.flags & (1u << 23)) != 0u) {
        emissive *= clamp(fragColor.rgb, 0.0, 1.0);
    }
    // Added to `color` by the tail, after the sheen and clearcoat layers, so neither
    // attenuates it.
