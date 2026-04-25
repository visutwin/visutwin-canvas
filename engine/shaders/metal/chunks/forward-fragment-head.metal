// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
fragment float4 VT_FRAGMENT_ENTRY(RasterizerData rd [[stage_in]],
                                  constant MaterialData &material [[buffer(3)]],
                                  constant LightingData &lighting [[buffer(4)]],
                                  texture2d<float> baseColorTexture [[texture(0)]],
                                  texture2d<float> normalTexture [[texture(1)]],
                                  texture2d<float> envAtlasTexture [[texture(2)]],
                                  texture2d<float> metallicRoughnessTexture [[texture(3)]],
                                  texture2d<float> occlusionTexture [[texture(4)]],
                                  texture2d<float> emissiveTexture [[texture(5)]],
#if VT_FEATURE_VSM_SHADOWS
                                  // Directional EVSM_16F: shadow map is a 2D moments
                                  // texture (RGBA16F) sampled with bilinear/Chebyshev,
                                  // not a hardware-compared depth texture.
                                  texture2d<float> shadowTexture [[texture(6)]],
#else
                                  depth2d<float> shadowTexture [[texture(6)]],
#endif
#if VT_FEATURE_SKY_CUBEMAP
                                  texturecube<float> skyboxCubeMap [[texture(8)]],
#endif
#if VT_FEATURE_PLANAR_REFLECTION
                                  texture2d<float> reflectionTexture [[texture(9)]],
                                  texture2d<float> reflectionDepthTexture [[texture(10)]],
#endif
#if VT_FEATURE_LOCAL_SHADOWS
                                  depth2d<float> localShadowTexture0 [[texture(11)]],
                                  depth2d<float> localShadowTexture1 [[texture(12)]],
#endif
#if VT_FEATURE_CLEARCOAT
                                  texture2d<float> clearCoatTexture [[texture(7)]],
                                  texture2d<float> clearCoatGlossTexture [[texture(13)]],
                                  texture2d<float> clearCoatNormalTexture [[texture(14)]],
#endif
#if VT_FEATURE_OMNI_SHADOWS
                                  depthcube<float> omniShadowCube0 [[texture(15)]],
                                  depthcube<float> omniShadowCube1 [[texture(16)]],
#endif
#if VT_FEATURE_PARALLAX
                                  texture2d<float> heightMapTexture [[texture(17)]],
#endif
#if VT_FEATURE_LIGHT_CLUSTERING
                                  device const ClusteredLight* clusterLights [[buffer(7)]],
                                  device const uchar* clusterCells [[buffer(8)]],
#endif
#if VT_FEATURE_SSAO
                                  texture2d<float> ssaoTexture [[texture(18)]],
#endif
#if VT_FEATURE_ATMOSPHERE
                                  constant AtmosphereData &atmosphere [[buffer(9)]],
#endif
                                  sampler defaultSampler [[sampler(0)]],
                                  bool isFrontFace [[front_facing]])
{
#if VT_FEATURE_SKYBOX

    // Compute sky direction from the pre-transform local vertex position,
    // carried in worldNormal (repurposed — skybox doesn't need surface normals).
    // Using worldPos - cameraPosition would suffer catastrophic float32 cancellation
    // at globe scale (both values ~10M meters, difference ~1 meter).
    // For SKY_DOME, subtract the dome center so the flattened bottom hemisphere
    // projects as a ground plane (tripod projection).
    const bool isDome = (lighting.skyDomeCenter.w > 0.5);
    const float3 viewDir = isDome
        ? normalize(rd.worldPos - lighting.skyDomeCenter.xyz)
        : normalize(rd.worldNormal);

#if VT_FEATURE_ATMOSPHERE
    // Nishita atmospheric scattering — replaces cubemap/atlas for sky visual.
    {
        const float3 skyLinear = nishitaScatter(viewDir, atmosphere);
        if ((lighting.flagsAndPad.x & (1u << 5)) != 0u) {
            return float4(max(skyLinear, float3(0.0)), 1.0);
        }
        const float exposure = max(lighting.skyboxMipAndPad.y, 0.0);
        const float tonemapMode = lighting.skyboxMipAndPad.z;
        return float4(linearToSrgb(toneMap(max(skyLinear, float3(0.0)), exposure, tonemapMode)), 1.0);
    }
#elif VT_FEATURE_SKY_CUBEMAP
    // SKY_CUBEMAP path — sample high-res cubemap
    if (skyboxCubeMap.get_width() > 0) {
        float3 dir = viewDir;
        dir.x *= -1.0;
        const float4 raw = skyboxCubeMap.sample(defaultSampler, dir);
        const float3 skyLinear = processEnvironment(decodeEnvironment(raw, lighting), max(lighting.cameraPositionSkyboxIntensity.w, 0.0));
        if ((lighting.flagsAndPad.x & (1u << 5)) != 0u) {
            return float4(max(skyLinear, float3(0.0)), 1.0);
        }
        const float exposure = max(lighting.skyboxMipAndPad.y, 0.0);
        const float tonemapMode = lighting.skyboxMipAndPad.z;
        return float4(linearToSrgb(toneMap(max(skyLinear, float3(0.0)), exposure, tonemapMode)), 1.0);
    }
#else
    // envAtlas path — sample 2D environment atlas
    //
    // Matches upstream skybox.js sampling: one texture fetch, default
    // sampler, no runtime seam detection. The atlas is baked with a 1-pixel
    // duplicated border at every rect edge (see envLighting.cpp
    // `seamPixels = 1.0f`), so bilinear/anisotropic filtering at u≈0 / u≈1
    // naturally samples continuous values across the ±180° longitudinal
    // wrap. Runtime `isAtEnvSeam` branching would add a second sampling
    // mode and introduce filter-kernel-mismatch artifacts at the band
    // boundaries — don't do it.
    if (envAtlasTexture.get_width() > 0 && envAtlasTexture.get_height() > 0) {
        const float3 dir = viewDir * float3(-1.0, 1.0, 1.0);
        const float skyMip = max(lighting.skyboxMipAndPad.x, 0.0);
        const float skyInt = max(lighting.cameraPositionSkyboxIntensity.w, 0.0);
        const float2 uv = toSphericalUv(normalize(dir));
        const float3 skyLinear = processEnvironment(decodeEnvironment(
            envAtlasTexture.sample(defaultSampler, mapRoughnessUv(uv, skyMip)), lighting), skyInt);
        // when CameraFrame is active (bit 5 of flagsAndPad.x),
        // skybox outputs linear HDR — tonemapping and gamma are deferred to the compose pass.
        if ((lighting.flagsAndPad.x & (1u << 5)) != 0u) {
            return float4(max(skyLinear, float3(0.0)), 1.0);
        }
        const float exposure = max(lighting.skyboxMipAndPad.y, 0.0);
        const float tonemapMode = lighting.skyboxMipAndPad.z;
        return float4(linearToSrgb(toneMap(max(skyLinear, float3(0.0)), exposure, tonemapMode)), 1.0);
    }
#endif
    return float4(0.0, 0.0, 0.0, 1.0);
#else
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

#if VT_FEATURE_INSTANCING
    // Instanced path: per-instance color from vertex shader overrides material baseColor.
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
    baseLinear *= saturate(rd.vertexColor.rgb);
    alpha *= saturate(rd.vertexColor.a);
#endif

#if VT_FEATURE_ALPHA_TEST
    if (alpha < material.alphaCutoff) {
        discard_fragment();
    }
#endif

#if VT_FEATURE_UNLIT
    // KHR_materials_unlit: output base color directly, skip all PBR lighting.
    {
        const float3 unlitColor = baseLinear;
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

#if VT_FEATURE_NORMAL_MAP
    if (normalTexture.get_width() > 0 && normalTexture.get_height() > 0) {
        float3 normalSample = normalTexture.sample(defaultSampler, uvNormal).xyz * 2.0 - 1.0;
        // blend toward flat (0,0,1) by bumpiness/normalScale.
        // At normalScale=1.0 → full normal map; at 0.0 → geometric surface normal.
        //mix(vec3(0,0,1), normalMap, material_bumpiness).
        normalSample = normalize(mix(float3(0.0, 0.0, 1.0), normalSample, material.normalScale));
        float3 T = rd.worldTangent.xyz;
        if (length_squared(T) >= 1e-6) {
            T = normalize(T);
            float3 B = normalize(cross(N, T)) * rd.worldTangent.w;
            const float3x3 tbn = float3x3(T, B, N);
            N = normalize(tbn * normalSample);
        }
    }
#endif

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
