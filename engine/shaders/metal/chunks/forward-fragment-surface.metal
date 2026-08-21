// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
    // Select UV set, then apply per-texture 3×2 affine transform (tiling, offset, rotation).
    float2 uvBase = ((material.flags & (1u << 4)) != 0u) ? rd.uv1 : rd.uv0;
    uvBase = applyUvTransform(uvBase, material.baseColorTransform0, material.baseColorTransform1);
    float2 uvNormal = ((material.flags & (1u << 5)) != 0u) ? rd.uv1 : rd.uv0;
    uvNormal = applyUvTransform(uvNormal, material.normalTransform0, material.normalTransform1);
    float2 uvMetalRough = ((material.flags & (1u << 7)) != 0u) ? rd.uv1 : rd.uv0;
    uvMetalRough = applyUvTransform(uvMetalRough, material.metalRoughTransform0, material.metalRoughTransform1);
    float2 uvOcclusion = ((material.flags & (1u << 10)) != 0u) ? rd.uv1 : rd.uv0;
    uvOcclusion = applyUvTransform(uvOcclusion, material.occlusionTransform0, material.occlusionTransform1);
    float2 uvEmissive = ((material.flags & (1u << 12)) != 0u) ? rd.uv1 : rd.uv0;
    uvEmissive = applyUvTransform(uvEmissive, material.emissiveTransform0, material.emissiveTransform1);

#if VT_FEATURE_PARALLAX
    // Parallax Occlusion Mapping: offset all texture UVs before any sampling.
    // Parallax mapping, enhanced with multi-step POM for quality close-ups.
    if ((material.flags & (1u << 17)) != 0u && heightMapTexture.get_width() > 0
        && material.heightMapFactor > 0.0) {
        // Build tangent-space view direction from vertex TBN.
        const float3 N_geom = normalize(rd.worldNormal);
        const float3 V_par = normalize(lighting.cameraPositionSkyboxIntensity.xyz - rd.worldPos);
        float3 T_par = rd.worldTangent.xyz;
        if (length_squared(T_par) >= 1e-6) {
            T_par = normalize(T_par);
            const float3 B_par = normalize(cross(N_geom, T_par)) * rd.worldTangent.w;
            // dot(basis, V) transforms world-space V into tangent space.
            const float3 viewDirTS = normalize(
                float3(dot(T_par, V_par), dot(B_par, V_par), dot(N_geom, V_par)));

            const float2 pomUV = parallaxOcclusionMap(
                uvBase, viewDirTS, heightMapTexture, defaultSampler,
                material.heightMapFactor);
            const float2 uvDelta = pomUV - uvBase;

            uvBase       += uvDelta;
            uvNormal     += uvDelta;
            uvMetalRough += uvDelta;
            uvOcclusion  += uvDelta;
            uvEmissive   += uvDelta;
        }
    }
#endif

#if VT_FEATURE_INSTANCING_COLOR
    // Instanced path with an 80-byte instance stride: the per-instance color from the
    // vertex shader overrides material baseColor. A matrix-only instance buffer takes
    // the #else branch and uses the material's base color, like any other draw.
    float alpha = rd.instanceColor.a;
    float3 baseLinear = srgbToLinear(rd.instanceColor.rgb);
#else
    float alpha = material.baseColor.a;
    float3 baseLinear = srgbToLinear(material.baseColor.rgb);
#endif

#if VT_FEATURE_BASE_COLOR_MAP
    if (baseColorTexture.get_width() > 0 && baseColorTexture.get_height() > 0) {
        const float4 baseSample = baseColorTexture.sample(defaultSampler, uvBase);
        baseLinear *= srgbToLinear(baseSample.rgb);
        alpha *= baseSample.a;
    }
#endif

#if VT_FEATURE_VERTEX_COLORS
    // Modulate base color by interpolated vertex color (already linearized in VS).
    // upstream convention: RGB modulates diffuse, A modulates opacity.
    // Upstream splits this across `diffuseVertexColor` and `emissiveVertexColor`;
    // here the diffuse lane is on by default (bit 28 turns it OFF, so existing
    // materials keep their behaviour) and bit 23 routes the color to emissive
    // instead — see forward-fragment-emissive.
    if ((material.flags & (1u << 28)) == 0u) {
        baseLinear *= saturate(rd.vertexColor.rgb);
    }
    alpha *= saturate(rd.vertexColor.a);
#endif

#if VT_FEATURE_DEBUG_PASS
    // DEBUGPASS_LIGHTING (upstream debug-process-frontend.js): neutralize albedo before it feeds
    // diffuseColor/F0, so the lit output below shows the lighting alone rather than the texture.
    // Unlike the other debug modes this one does not replace the output — it falls through to the
    // regular lit path in the tail chunk.
    if (lighting.flagsAndPad.y == VT_DEBUGPASS_LIGHTING) {
        baseLinear = float3(0.5);
    }
#endif

#if VT_FEATURE_ALPHA_TEST
    if (alpha < material.alphaCutoff) {
        discard_fragment();
    }
#endif

#if VT_FEATURE_OPACITY_DITHER
    // Opacity dithering (upstream opacity-dither.js): screen-space ordered dither turns partial
    // opacity into a discard pattern so transparency renders in the opaque pass with correct
    // depth. The matrix is chosen per material via flags bits 25-27 (DitherMode), a runtime value
    // rather than a shader variant. DEVIATION: no blue-noise / IGN variants and no per-frame
    // jitter (static pattern; upstream jitters for TAA convergence).
    {
        // Upstream's alphaDither decouples the two strengths: opacity keeps driving the alpha
        // blend while this value alone drives the dither density. Negative means unset, which
        // restores the coupled behaviour every material had before.
        const float ditherStrength = material.dispersionParams.y;
        const bool hasAlphaDither = ditherStrength >= 0.0;
        const float ditherAlpha = hasAlphaDither ? ditherStrength : alpha;

        if (ditherDiscards((material.flags >> 25) & 0x7u, rd.position.xy, ditherAlpha)) {
            discard_fragment();
        }

        // Coupled (legacy) use is an opaque-pass technique: the surviving fragments are fully
        // opaque, and forcing alpha keeps the target's alpha channel clean. Decoupled use is
        // upstream's blend-AND-dither case, where alpha must survive to drive the blend.
        if (!hasAlphaDither) {
            alpha = 1.0;
        }
    }
#endif

#if VT_FEATURE_UNLIT
    // KHR_materials_unlit: output base color directly, skip all PBR lighting.
    {
        // Emissive still contributes — upstream reaches this path through
        // `useLighting = false`, which drops the lights but keeps the emissive
        // lane (that is how its decal material draws at all: black diffuse plus a
        // bright emissive map). Recomputed here rather than deferred to
        // forward-fragment-emissive, which this early return never reaches.
        float3 unlitEmissive = max(material.emissiveColor.rgb, float3(0.0));
#if VT_FEATURE_EMISSIVE_MAP
        if (emissiveTexture.get_width() > 0 && emissiveTexture.get_height() > 0) {
            unlitEmissive *= srgbToLinear(emissiveTexture.sample(defaultSampler, uvEmissive).rgb);
        }
#endif
#if VT_FEATURE_VERTEX_COLORS
        if ((material.flags & (1u << 23)) != 0u) {
            unlitEmissive *= saturate(rd.vertexColor.rgb);
        }
#endif
        const float3 unlitColor = baseLinear + unlitEmissive;
        if ((lighting.flagsAndPad.x & (1u << 5)) != 0u) {
            return float4(max(unlitColor, float3(0.0)), alpha);
        }
        const float exposure = max(lighting.skyboxMipAndPad.y, 0.0);
        const float tonemapMode = lighting.skyboxMipAndPad.z;
        return float4(linearToSrgb(toneMap(max(unlitColor, float3(0.0)), exposure, tonemapMode)), alpha);
    }
#endif

    float3 N = normalize(rd.worldNormal);
    const float3 cameraPosition = lighting.cameraPositionSkyboxIntensity.xyz;
    const float3 V = normalize(cameraPosition - rd.worldPos);

#if VT_FEATURE_DOUBLE_SIDED
    if (!isFrontFace) {
        N = -N;
    }
#endif

#if VT_FEATURE_NORMAL_MAP || VT_FEATURE_DETAIL_NORMALS
    {
        float3 normalSample = float3(0.0, 0.0, 1.0);
        bool haveSample = false;
#if VT_FEATURE_NORMAL_MAP
        if (normalTexture.get_width() > 0 && normalTexture.get_height() > 0) {
            normalSample = normalTexture.sample(defaultSampler, uvNormal).xyz * 2.0 - 1.0;
            // blend toward flat (0,0,1) by bumpiness/normalScale.
            // At normalScale=1.0 → full normal map; at 0.0 → geometric surface normal.
            normalSample = normalize(mix(float3(0.0, 0.0, 1.0), normalSample, material.normalScale));
            haveSample = true;
        }
#endif
#if VT_FEATURE_DETAIL_NORMALS
        // Detail normal overlay (UDN blend): the detail map's xy perturbation is
        // scaled by detailNormalScale and added in tangent space on top of the
        // base normal sample (or the flat normal when no base map is bound).
        if (detailNormalTexture.get_width() > 0 && detailNormalTexture.get_height() > 0) {
            const float2 uvDetail = applyUvTransform(rd.uv0,
                material.detailNormalTransform0, material.detailNormalTransform1);
            float3 detailSample = detailNormalTexture.sample(defaultSampler, uvDetail).xyz * 2.0 - 1.0;
            detailSample.xy *= material.detailDisplacementParams.x;  // detailNormalScale
            normalSample = normalize(float3(normalSample.xy + detailSample.xy, normalSample.z));
            haveSample = true;
        }
#endif
        if (haveSample) {
            float3 T = rd.worldTangent.xyz;
            if (length_squared(T) >= 1e-6) {
                T = normalize(T);
                float3 B = normalize(cross(N, T)) * rd.worldTangent.w;
                const float3x3 tbn = float3x3(T, B, N);
                N = normalize(tbn * normalSample);
            }
        }
    }
#endif

#if VT_FEATURE_SPEC_GLOSS
    // KHR_materials_pbrSpecularGlossiness: diffuse color + specular color +
    // glossiness instead of metallic/roughness. The spec-gloss texture reuses the
    // metal-rough binding (slot 3): rgb = specular color (sRGB), a = glossiness.
    float3 specularColor = clamp(material.specGlossParams.rgb, 0.0, 1.0);
    float glossiness = clamp(material.specGlossParams.w, 0.0, 1.0);
    if ((material.flags & (1u << 21)) != 0u && metallicRoughnessTexture.get_width() > 0) {
        const float4 sg = metallicRoughnessTexture.sample(defaultSampler, uvMetalRough);
        specularColor *= srgbToLinear(sg.rgb);
        glossiness *= sg.a;
    }
    const float metallic = 0.0;
    const float roughness = clamp(1.0 - glossiness, 0.04, 1.0);
    // Diffuse energy conservation per the extension: scale by 1 - max(specular).
    const float3 diffuseColor = baseLinear *
        (1.0 - max(specularColor.r, max(specularColor.g, specularColor.b)));
    const float3 F0 = specularColor;
#else
    float metallic = clamp(material.metallicFactor, 0.0, 1.0);
    float roughness = clamp(material.roughnessFactor, 0.04, 1.0);
#if VT_FEATURE_METAL_ROUGHNESS_MAP
    if (metallicRoughnessTexture.get_width() > 0 && metallicRoughnessTexture.get_height() > 0) {
        const float4 mr = metallicRoughnessTexture.sample(defaultSampler, uvMetalRough);
        roughness = clamp(roughness * mr.g, 0.04, 1.0);
        metallic = clamp(metallic * mr.b, 0.0, 1.0);
    }
#endif

    const float3 diffuseColor = baseLinear * (1.0 - metallic);
    const float3 F0 = mix(float3(0.04), baseLinear, metallic);
#endif
    const float gloss = 1.0 - roughness;

#if VT_FEATURE_CLEARCOAT
    // Clearcoat, clearcoat gloss, and clearcoat normal.
    // Clearcoat dual-layer BRDF: thin dielectric coat (IOR 1.5, F0=0.04) over PBR base.
    float ccSpecularity = material.clearCoatFactor;
    float ccGlossiness = 1.0 - clamp(material.clearCoatRoughness, 0.0, 1.0);

    // Sample clearcoat intensity map (green channel, upstream convention).
    if ((material.flags & (1u << 14)) != 0u && clearCoatTexture.get_width() > 0) {
        ccSpecularity *= clearCoatTexture.sample(defaultSampler, uvBase).g;
    }

    // Sample clearcoat gloss map (green channel).
    if ((material.flags & (1u << 15)) != 0u && clearCoatGlossTexture.get_width() > 0) {
        ccGlossiness *= clearCoatGlossTexture.sample(defaultSampler, uvBase).g;
    }

    ccGlossiness += 0.0000001; // prevent divide-by-zero

    // Clearcoat normal: use surface normal by default, override with clearcoat normal map.
    float3 ccNormalW = N;
    if ((material.flags & (1u << 16)) != 0u && clearCoatNormalTexture.get_width() > 0) {
        float3 ccNormalSample = clearCoatNormalTexture.sample(defaultSampler, uvNormal).xyz * 2.0 - 1.0;
        ccNormalSample = normalize(mix(float3(0.0, 0.0, 1.0), ccNormalSample, material.clearCoatBumpiness));
        float3 ccT = rd.worldTangent.xyz;
        if (length_squared(ccT) >= 1e-6) {
            ccT = normalize(ccT);
            float3 ccB = normalize(cross(N, ccT)) * rd.worldTangent.w;
            ccNormalW = normalize(float3x3(ccT, ccB, N) * ccNormalSample);
        }
    }

    // Clearcoat GGX alpha (Disney roughness remapping: alpha = roughness^2).
    const float ccRoughness = max(1.0 - ccGlossiness, 0.04);
    const float ccAlpha2 = ccRoughness * ccRoughness * ccRoughness * ccRoughness;

    // Accumulators for clearcoat direct and indirect lighting.
    float3 ccSpecularLight = float3(0.0);
    float3 ccReflection = float3(0.0);
#endif

#if VT_FEATURE_SHEEN
    // Sheen fabric BRDF accumulators (Charlie distribution + Ashikhmin visibility).
    // sheenColor.rgb = tint, sheenColor.w = roughness (0 = smooth, 1 = rough).
    const float3 sheenTint = srgbToLinear(material.sheenColor.rgb);
    const float sheenRoughness = max(material.sheenColor.w, 0.04);
    float3 sheenSpecularDirect = float3(0.0);
    float3 sheenSpecularIndirect = float3(0.0);
#endif

#if VT_FEATURE_IRIDESCENCE
    // Thin-film iridescence: compute modified Fresnel once before the lighting loop.
    // iridescenceParams: x=intensity, y=IOR, z=thicknessMin(nm), w=thicknessMax(nm).
    // Without a thickness map, use thicknessMax directly.
    const float iridIntensity = clamp(material.iridescenceParams.x, 0.0, 1.0);
    const float iridThickness = material.iridescenceParams.w;  // nm (thicknessMax)
    const float iridIor = max(material.iridescenceParams.y, 1.001);
    const float iridNdotV = max(dot(N, V), 0.001);
    const float3 iridFresnel = getIridescence(iridNdotV, F0, iridThickness, iridIor);
#endif

    float3 directDiffuse = float3(0.0);
    float3 directSpecular = float3(0.0);
    const uint lightCount = min(lighting.lightCountAndFlags.x, 8u);
#if VT_FEATURE_MULTI_LIGHT
    const uint loopLightCount = lightCount;
#else
    const uint loopLightCount = min(lightCount, 1u);
#endif
    const float roughnessSq = max((1.0 - gloss) * (1.0 - gloss), 0.001);
    const float alpha2 = roughnessSq * roughnessSq;
    // DEVIATION (upstream parity): the Smith-GGX height-correlated visibility
    // term in upstream lightSpecularGGX.js feeds `alpha2 = alpha * alpha`
    // into the Heitz lambda formula, which is one squaring beyond Heitz's
    // textbook form. Mathematically non-standard, but it widens G and makes
    // direct specular highlights pop more — particularly at grazing angles
    // (e.g. sunlight glinting off a curved metallic wing). Matching it here.
    const float alpha4 = alpha2 * alpha2;
    bool shadowApplied = false;
#if VT_FEATURE_SHADOW_CATCHER
    // shadow catcher accumulates shadow factors multiplicatively.
    // 1.0 = fully lit (no shadow), 0.0 = fully shadowed.
    float dShadowCatcher = 1.0;
#endif

#if VT_FEATURE_ANISOTROPY
    // Anisotropic GGX setup: tangent/bitangent and directional roughness.
    // Anisotropic specular GGX.
    float3 anisoT = float3(1.0, 0.0, 0.0);
    float3 anisoB = float3(0.0, 0.0, 1.0);
    float anisoAt = roughnessSq;   // default = isotropic alpha
    float anisoAb = roughnessSq;
    {
        const float aniso = clamp(material.anisotropy, -1.0, 1.0);
        float3 T = rd.worldTangent.xyz;
        if (length_squared(T) >= 1e-6) {
            T = normalize(T);
            anisoT = T;
            anisoB = normalize(cross(N, T)) * rd.worldTangent.w;
        }
        // αt, αb from Disney alpha (roughness²) scaled by anisotropy factor.
        // Seamlessly reduces to isotropic when aniso=0 (αt=αb=roughnessSq).
        anisoAt = max(roughnessSq * (1.0 + aniso), 0.001);
        anisoAb = max(roughnessSq * (1.0 - aniso), 0.001);
    }
    const float anisoAt2 = anisoAt * anisoAt;
    const float anisoAb2 = anisoAb * anisoAb;
#endif
