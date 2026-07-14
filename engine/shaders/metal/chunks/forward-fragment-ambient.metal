// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
    // ambient diffuse (no energy conservation).
#if VT_FEATURE_LIGHT_PROBES
    // Ambient SH light probes: 9-coefficient irradiance evaluated in the world
    // normal direction (upstream AMBIENTSH basis, coefficients premultiplied).
    const float3 shN = N;
    float3 indirectDiffuse = max(
        lighting.ambientSH[0].xyz +
        lighting.ambientSH[1].xyz * shN.x +
        lighting.ambientSH[2].xyz * shN.y +
        lighting.ambientSH[3].xyz * shN.z +
        lighting.ambientSH[4].xyz * (shN.x * shN.z) +
        lighting.ambientSH[5].xyz * (shN.z * shN.y) +
        lighting.ambientSH[6].xyz * (shN.y * shN.x) +
        lighting.ambientSH[7].xyz * (3.0 * shN.z * shN.z - 1.0) +
        lighting.ambientSH[8].xyz * (shN.x * shN.x - shN.y * shN.y),
        float3(0.0));
#else
    float3 indirectDiffuse = max(lighting.ambientColor.xyz, float3(0.0));
#endif
    float3 indirectSpecular = float3(0.0);
#if VT_FEATURE_ENV_ATLAS
    if (envAtlasTexture.get_width() > 0 && envAtlasTexture.get_height() > 0) {
#if !VT_FEATURE_LIGHT_PROBES
        // Diffuse IBL: sample from dedicated Lambert irradiance sub-region
        // through `envAtlasSampler` (non-anisotropic, see common.metal).
        // Skipped when SH probes drive the ambient (probes take priority).
        const float3 diffDir = float3(-N.x, N.y, N.z);
        const float2 envUvN = toSphericalUv(normalize(diffDir));
        const float3 envAmbient = processEnvironment(
            decodeEnvironment(envAtlasTexture.sample(envAtlasSampler, mapAmbientUv(envUvN)), lighting),
            max(lighting.cameraPositionSkyboxIntensity.w, 0.0));
        indirectDiffuse = envAmbient;
#endif

        // Specular IBL: dual-path sampling.
        // Glossy (level==0): sample from shiny atlas sub-region with screen-space MIP.
        // Rough (level>0): trilinearly interpolate between adjacent roughness MIP levels.
#if VT_FEATURE_ANISOTROPY
        // Bend reflection toward bitangent
        // for anisotropic IBL. Elongates reflections along the anisotropy direction.
        const float3 R = reflect(-V, normalize(mix(N, anisoB, material.anisotropy)));
#else
        const float3 R = reflect(-V, N);
#endif
        const float3 specDir = float3(-R.x, R.y, R.z);
        const float2 envUvSpec = toSphericalUv(normalize(specDir));

        const float level = saturate(1.0 - gloss) * 5.0;
        const float ilevel = floor(level);

        // Screen-space MIP level for shiny path (shinyMipLevel).
        const float2 shinyUvFull = envUvSpec * ATLAS_SIZE;
        const float2 dx = dfdx(shinyUvFull);
        const float2 dy = dfdy(shinyUvFull);
        // Handle discontinuity at azimuthal edge.
        const float2 uv2 = float2(fract(envUvSpec.x + 0.5), envUvSpec.y) * ATLAS_SIZE;
        const float2 dx2 = dfdx(uv2);
        const float2 dy2 = dfdy(uv2);
        const float maxd = min(max(dot(dx, dx), dot(dy, dy)), max(dot(dx2, dx2), dot(dy2, dy2)));
        const float level2 = clamp(0.5 * log2(maxd) - 1.0, 0.0, 5.0);
        const float ilevel2 = floor(level2);

        // Specular IBL: sampled through `envAtlasSampler` (non-anisotropic,
        // see common.metal). The upstream `shinyMipLevel` uses a
        // second-derivative trick (dFdx/dFdy on fract(u+0.5)) above to pick
        // the correct screen-space MIP across the wrap.
        float3 linear0, linear1;
        if (ilevel == 0.0) {
            linear0 = decodeEnvironment(envAtlasTexture.sample(envAtlasSampler, mapShinyUv(envUvSpec, ilevel2)), lighting);
            linear1 = decodeEnvironment(envAtlasTexture.sample(envAtlasSampler, mapShinyUv(envUvSpec, ilevel2 + 1.0)), lighting);
            linear0 = mix(linear0, linear1, level2 - ilevel2);
        } else {
            linear0 = decodeEnvironment(envAtlasTexture.sample(envAtlasSampler, mapRoughnessUv(envUvSpec, ilevel)), lighting);
        }
        linear1 = decodeEnvironment(envAtlasTexture.sample(envAtlasSampler, mapRoughnessUv(envUvSpec, ilevel + 1.0)), lighting);

        const float3 envSpec = processEnvironment(mix(linear0, linear1, level - ilevel),
            max(lighting.cameraPositionSkyboxIntensity.w, 0.0));

        // gloss-dependent Fresnel on reflections.
        float3 fresnelNV = getFresnel(dot(N, V), gloss, F0);
#if VT_FEATURE_IRIDESCENCE
        // Thin-film iridescence: blend IBL Fresnel toward iridescence Fresnel.
        fresnelNV = mix(fresnelNV, iridFresnel, iridIntensity);
#endif
        indirectSpecular = envSpec * fresnelNV;

#if VT_FEATURE_CLEARCOAT
        // Clearcoat IBL reflection.
        // Sample environment at clearcoat roughness level using clearcoat normal.
        {
            const float3 ccR = reflect(-V, ccNormalW);
            const float2 ccEnvUv = toSphericalUv(normalize(float3(-ccR.x, ccR.y, ccR.z)));
            const float ccLevel = saturate(1.0 - ccGlossiness) * 5.0;
            const float ccIlevel = floor(ccLevel);

            float3 ccEnvColor;
            if (ccIlevel == 0.0) {
                const float3 a = decodeEnvironment(envAtlasTexture.sample(envAtlasSampler, mapShinyUv(ccEnvUv, 0.0)), lighting);
                const float3 b = decodeEnvironment(envAtlasTexture.sample(envAtlasSampler, mapShinyUv(ccEnvUv, 1.0)), lighting);
                ccEnvColor = mix(a, b, ccLevel);
            } else {
                const float3 a = decodeEnvironment(envAtlasTexture.sample(envAtlasSampler, mapRoughnessUv(ccEnvUv, ccIlevel)), lighting);
                const float3 b = decodeEnvironment(envAtlasTexture.sample(envAtlasSampler, mapRoughnessUv(ccEnvUv, ccIlevel + 1.0)), lighting);
                ccEnvColor = mix(a, b, ccLevel - ccIlevel);
            }

            const float ccNdotV = max(dot(ccNormalW, V), 0.0);
            ccReflection = processEnvironment(ccEnvColor,
                max(lighting.cameraPositionSkyboxIntensity.w, 0.0)) * getFresnelCC(ccNdotV);
        }
#endif

#if VT_FEATURE_SHEEN
        // Sheen IBL: analytical approximation (no LUT texture).
        // Samples the environment atlas at sheen roughness level using the reflected direction.
        {
            const float3 sheenR = reflect(-V, N);
            const float2 sheenEnvUv = toSphericalUv(normalize(float3(-sheenR.x, sheenR.y, sheenR.z)));
            const float sheenLevel = saturate(sheenRoughness) * 5.0;
            const float sheenILevel = floor(sheenLevel);

            float3 sheenEnvColor;
            if (sheenILevel == 0.0) {
                const float3 a = decodeEnvironment(envAtlasTexture.sample(envAtlasSampler, mapShinyUv(sheenEnvUv, 0.0)), lighting);
                const float3 b = decodeEnvironment(envAtlasTexture.sample(envAtlasSampler, mapShinyUv(sheenEnvUv, 1.0)), lighting);
                sheenEnvColor = mix(a, b, sheenLevel);
            } else {
                const float3 a = decodeEnvironment(envAtlasTexture.sample(envAtlasSampler, mapRoughnessUv(sheenEnvUv, sheenILevel)), lighting);
                const float3 b = decodeEnvironment(envAtlasTexture.sample(envAtlasSampler, mapRoughnessUv(sheenEnvUv, sheenILevel + 1.0)), lighting);
                sheenEnvColor = mix(a, b, sheenLevel - sheenILevel);
            }

            // Analytical directional albedo approximation (replaces LUT).
            const float sheenNdotV = max(dot(N, V), 0.001);
            const float sheenE = sheenIBLApprox(sheenNdotV, sheenRoughness);

            sheenSpecularIndirect = processEnvironment(sheenEnvColor,
                max(lighting.cameraPositionSkyboxIntensity.w, 0.0)) * sheenTint * sheenE;
        }
#endif
    }
#endif

#if VT_FEATURE_REFLECTION_PROBE
    // Local reflection probe: sample a prefiltered cubemap in the reflection
    // direction, optionally box-projected (parallax-corrected) so reflections
    // align to the probe's volume. Overrides the global env-atlas specular.
    if (reflectionProbeCube.get_width() > 0) {
        const float3 Rp = reflect(-V, N);
        float3 sampleDir = Rp;

        // Box projection (upstream cubeMapProject BOX): intersect the reflection
        // ray with the probe box and re-aim from the box center — this is what
        // makes a flat cubemap track a room's walls as the surface moves.
        if (lighting.reflectionProbeParams.x > 0.5) {
            const float3 boxMin = lighting.reflectionProbeBoxMin.xyz;
            const float3 boxMax = lighting.reflectionProbeBoxMax.xyz;
            const float3 invDir = 1.0 / Rp;
            const float3 rbmax = (boxMax - rd.worldPos) * invDir;
            const float3 rbmin = (boxMin - rd.worldPos) * invDir;
            const float3 rbminmax = select(rbmin, rbmax, Rp > 0.0);
            const float fa = min(min(rbminmax.x, rbminmax.y), rbminmax.z);
            const float3 posOnBox = rd.worldPos + Rp * fa;
            const float3 boxCenter = (boxMin + boxMax) * 0.5;
            sampleDir = normalize(posOnBox - boxCenter);
        }

        // Engine cube convention flips X (matches the skybox cube sampling).
        const float3 cubeDir = float3(-sampleDir.x, sampleDir.y, sampleDir.z);
        // Roughness → mip LOD (DEVIATION: hardware trilinear mips approximate the
        // GGX prefilter upstream bakes per level).
        const float probeLod = saturate(1.0 - gloss) * lighting.reflectionProbeParams.z;
        float3 probeSpec = reflectionProbeCube.sample(reflectionProbeSampler, cubeDir, level(probeLod)).rgb;
        probeSpec = pow(max(probeSpec, float3(0.0)), float3(2.2)) * lighting.reflectionProbeParams.y;

        float3 probeFresnel = getFresnel(dot(N, V), gloss, F0);
#if VT_FEATURE_IRIDESCENCE
        probeFresnel = mix(probeFresnel, iridFresnel, iridIntensity);
#endif
        indirectSpecular = probeSpec * probeFresnel;
    }
#endif

#if VT_FEATURE_SSR
    // Screen-space reflections: march the reflection ray against the scene depth
    // grab and sample the scene color grab at the hit, blending OVER the
    // probe/env-atlas fallback where the ray hits on-screen geometry.
    if (ssrSceneDepthTexture.get_width() > 0 && sceneColorTexture.get_width() > 0) {
        const float ssrNear = lighting.cameraNearFar.x;
        const float ssrFar = lighting.cameraNearFar.y;
        const float3 ssrR = reflect(-V, N);

        const int SSR_STEPS = 48;
        const float SSR_MAX_DIST = 60.0;
        const float ssrStep = SSR_MAX_DIST / float(SSR_STEPS);
        const float ssrThickness = 1.5;   // view-space hit tolerance (world units)

        float2 ssrHitUv = float2(0.0);
        float ssrHit = 0.0;
        for (int i = 1; i <= SSR_STEPS; ++i) {
            const float3 samplePos = rd.worldPos + ssrR * (ssrStep * float(i));
            const float4 clip = lighting.viewProjection * float4(samplePos, 1.0);
            if (clip.w <= 0.0) break;                       // behind the camera
            const float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
            if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break;   // left the screen
            const float marchedZ = clip.w;                  // view-space distance
            const float rawDepth = ssrSceneDepthTexture.sample(envAtlasSampler, uv);
            const float sceneZ = (ssrNear * ssrFar) / (ssrFar - rawDepth * (ssrFar - ssrNear));
            const float diff = marchedZ - sceneZ;           // >0 = marched behind the surface
            if (diff > 0.0 && diff < ssrThickness) {
                ssrHitUv = uv;
                ssrHit = 1.0;
                break;
            }
        }

        if (ssrHit > 0.0) {
            float3 ssrColor = sceneColorTexture.sample(envAtlasSampler, ssrHitUv).rgb;
            // Decode the grab (tonemapped sRGB, unless the HDR camera-frame path).
            if ((lighting.flagsAndPad.x & (1u << 5)) == 0u) {
                ssrColor = pow(max(ssrColor, float3(0.0)), float3(2.2));
            }
            // Fade at screen edges (reflection pops as rays exit the frame) and on
            // rough surfaces (this port marches sharp — no roughness cone).
            const float2 eLo = smoothstep(float2(0.0), float2(0.12), ssrHitUv);
            const float2 eHi = 1.0 - smoothstep(float2(0.88), float2(1.0), ssrHitUv);
            const float edgeFade = eLo.x * eLo.y * eHi.x * eHi.y;
            const float roughFade = saturate(gloss * 1.2 - 0.2);
            const float3 ssrFresnel = getFresnel(dot(N, V), gloss, F0);
            indirectSpecular = mix(indirectSpecular, ssrColor * ssrFresnel, edgeFade * roughFade);
        }
    }
#endif

    float ao = 1.0;
#if VT_FEATURE_OCCLUSION_MAP
    if (occlusionTexture.get_width() > 0 && occlusionTexture.get_height() > 0) {
        const float occ = occlusionTexture.sample(defaultSampler, uvOcclusion).r;
        ao = mix(1.0, occ, clamp(material.occlusionStrength, 0.0, 1.0));
    }
#endif

#if VT_FEATURE_SSAO
    // Per-material SSAO: sample screen-space AO texture and multiply into ao.
    // screenInvResolution.xy = 1/width, 1/height — converts fragment pixel coords to [0,1] UV.
    if (ssaoTexture.get_width() > 0) {
        ao *= ssaoTexture.sample(defaultSampler, rd.position.xy * lighting.screenInvResolution.xy).r;
    }
#endif

    // AO diffuse/specular occlusion are applied separately (not as global output multiply).
    if ((material.flags & (1u << 13)) != 0u) {
        directDiffuse *= ao;
    }

    if (material.occludeSpecularMode != SPECOCC_NONE) {
        float specOcc = 1.0;
        if (material.occludeSpecularMode == SPECOCC_AO) {
            specOcc = ao;
        } else if (material.occludeSpecularMode == SPECOCC_GLOSSDEPENDENT) {
            const float specPow = exp2(gloss * 11.0);
            specOcc = saturate(pow(max(dot(N, V), 0.0) + ao, 0.01 * specPow) - 1.0 + ao);
        }

        const float specOccIntensity = clamp(material.occludeSpecularIntensity, 0.0, 1.0);
        specOcc = mix(1.0, specOcc, specOccIntensity);
        directSpecular *= specOcc;
        indirectSpecular *= specOcc;
#if VT_FEATURE_SHEEN
        sheenSpecularDirect *= specOcc;
        sheenSpecularIndirect *= specOcc;
#endif
    }

    // DEVIATION: material.emissiveColor is supplied as already-linear HDR (intensity pre-applied
    // in linear space by StandardMaterial::updateUniforms). Applying srgbToLinear here would
    // overflow fp16 for bright emissives. Only the texture sample (authored in sRGB) is decoded.
