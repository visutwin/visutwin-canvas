    // Emissive.
    vec3 emissive = material.emissiveColor.rgb;
    if (vtFeatureEnabled(VT_FEATURE_EMISSIVE_MAP_BIT)) {
        // The emissive TEXTURE is authored in sRGB and owes a decode, exactly like
        // the base-colour map. The FACTOR does not: StandardMaterial::updateUniforms
        // already pre-linearises setEmissive, which is why only this half is decoded
        // (same split as forward-fragment-emissive.metal). Missing this made every
        // emissive surface here about 2.3x too bright in linear terms.
        emissive *= srgbToLinear(texture(emissiveMap, uvEmissive).rgb);
    }
    // upstream `emissiveVertexColor` (bit 23): the vertex color tints emissive.
    if ((material.flags & (1u << 23)) != 0u) {
        emissive *= clamp(fragColor.rgb, 0.0, 1.0);
    }
    color += emissive;
