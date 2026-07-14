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
#if VT_FEATURE_LIGHTMAP
                                  texture2d<float> lightMapTexture [[texture(19)]],
#endif
#if VT_FEATURE_AREA_LIGHTS
                                  texture2d<float> areaLightsLutTex1 [[texture(20)]],
                                  texture2d<float> areaLightsLutTex2 [[texture(21)]],
#endif
#if VT_FEATURE_DYNAMIC_REFRACTION
                                  texture2d<float> sceneColorTexture [[texture(22)]],
#endif
#if VT_FEATURE_DETAIL_NORMALS
                                  texture2d<float> detailNormalTexture [[texture(23)]],
#endif
#if VT_FEATURE_REFLECTION_PROBE
                                  texturecube<float> reflectionProbeCube [[texture(24)]],
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
    // envAtlas path — sample 2D environment atlas with the inline
    // `envAtlasSampler` (non-anisotropic trilinear, see common.metal). The
    // default 16× anisotropic sampler reads dfdx(U) at the atan2 wrap
    // (n.z<0, n.x≈0) as a giant footprint and produces a vertical line
    // anchored to world −Z. The pre-baked 1-pixel seam border covers
    // bilinear, but not anisotropic kernels — so we drop anisotropy here.
    if (envAtlasTexture.get_width() > 0 && envAtlasTexture.get_height() > 0) {
        const float3 dir = viewDir * float3(-1.0, 1.0, 1.0);
        const float skyMip = max(lighting.skyboxMipAndPad.x, 0.0);
        const float skyInt = max(lighting.cameraPositionSkyboxIntensity.w, 0.0);
        const float2 uv = toSphericalUv(normalize(dir));
        const float3 skyLinear = processEnvironment(decodeEnvironment(
            envAtlasTexture.sample(envAtlasSampler, mapRoughnessUv(uv, skyMip)), lighting), skyInt);
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
