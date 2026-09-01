    // Emissive.
    vec3 emissive = material.emissiveColor.rgb;
    if (vtFeatureEnabled(VT_FEATURE_EMISSIVE_MAP_BIT)) {
        emissive *= texture(emissiveMap, uvEmissive).rgb;
    }
    // upstream `emissiveVertexColor` (bit 23): the vertex color tints emissive.
    if ((material.flags & (1u << 23)) != 0u) {
        emissive *= clamp(fragColor.rgb, 0.0, 1.0);
    }
    color += emissive;
