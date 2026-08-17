// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
    float3 emissiveLinear = max(material.emissiveColor.rgb, float3(0.0));
#if VT_FEATURE_EMISSIVE_MAP
    if (emissiveTexture.get_width() > 0 && emissiveTexture.get_height() > 0) {
        emissiveLinear *= srgbToLinear(emissiveTexture.sample(defaultSampler, uvEmissive).rgb);
    }
#endif
#if VT_FEATURE_VERTEX_COLORS
    // upstream `emissiveVertexColor`: the vertex color tints the emissive lane
    // instead of the diffuse one (bit 23; bit 28 is what took it off diffuse).
    if ((material.flags & (1u << 23)) != 0u) {
        emissiveLinear *= saturate(rd.vertexColor.rgb);
    }
#endif
