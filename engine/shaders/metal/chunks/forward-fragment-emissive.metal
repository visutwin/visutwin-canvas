// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
    float3 emissiveLinear = max(material.emissiveColor.rgb, float3(0.0));
#if VT_FEATURE_EMISSIVE_MAP
    if (emissiveTexture.get_width() > 0 && emissiveTexture.get_height() > 0) {
        emissiveLinear *= srgbToLinear(emissiveTexture.sample(defaultSampler, uvEmissive).rgb);
    }
#endif
