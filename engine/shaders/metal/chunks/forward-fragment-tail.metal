// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers

#if VT_FEATURE_PLANAR_REFLECTION_DEPTH_PASS
    // litUserMainEndPS:
    // Output distance-from-reflection-plane as grayscale.
    // All PBR lighting computation is skipped — this shader variant is used by the
    // depth camera to produce a per-pixel distance map for distance-dependent blur.
    //
    // reflectionDepthParams.x = planeDistance (d = -dot(normal, pointOnPlane))
    // reflectionDepthParams.y = heightRange (normalization factor)
    float distFromPlane = abs(rd.worldPos.y + lighting.reflectionDepthParams.x)
                        / lighting.reflectionDepthParams.y;
    return float4(distFromPlane, distFromPlane, distFromPlane, 1.0);
#endif

    // Placeholders for future chunk ports (deliberately compile-time dead when disabled).
#if VT_FEATURE_LIGHT_PROBES || VT_FEATURE_SKINNING || \
    VT_FEATURE_MORPHS || VT_FEATURE_SPEC_GLOSS || VT_FEATURE_OREN_NAYAR || VT_FEATURE_DETAIL_NORMALS || \
    VT_FEATURE_DISPLACEMENT
    // DEVIATION: feature chunks are staged behind compile-time toggles and will be ported incrementally.
    // NOTE: VT_FEATURE_VERTEX_COLORS, VT_FEATURE_CLEARCOAT, VT_FEATURE_LIGHT_CLUSTERING, VT_FEATURE_PARALLAX, VT_FEATURE_ANISOTROPY, VT_FEATURE_TRANSMISSION, VT_FEATURE_SSAO, VT_FEATURE_SHEEN, VT_FEATURE_IRIDESCENCE, VT_FEATURE_ATMOSPHERE removed — now fully implemented.
#endif

#if VT_FEATURE_SHADOW_CATCHER
    // shadow catcher overrides the normal lighting output.
    // Output the accumulated shadow factor as a grayscale value.
    // With multiplicative blending (src * dst), white (1.0) = no shadow (no change),
    // dark values darken the framebuffer where shadows fall.
    return float4(float3(dShadowCatcher), 1.0);
#else
    float3 litLinear = diffuseColor * (directDiffuse + indirectDiffuse) + directSpecular + indirectSpecular + emissiveLinear;

#if VT_FEATURE_TRANSMISSION
    // Cubemap-based refraction.
    // Samples the environment atlas in the refracted direction with roughness blur.
    if (material.transmissionFactor > 0.0 && envAtlasTexture.get_width() > 0) {
        const float ior = max(material.refractionIndex, 1.001);
        const float3 refrDir = refract(-V, N, 1.0 / ior);

        if (length_squared(refrDir) > 0.0) {
            // Sample environment atlas at refracted direction with roughness-dependent MIP.
            // Same dual-path sampling as IBL reflection (shiny + rough).
            const float2 refrUv = toSphericalUv(normalize(float3(-refrDir.x, refrDir.y, refrDir.z)));
            const float refrLevel = saturate(1.0 - gloss) * 5.0;
            const float refrILevel = floor(refrLevel);

            float3 refrSample;
            if (refrILevel == 0.0) {
                const float3 a = decodeEnvironment(envAtlasTexture.sample(defaultSampler, mapShinyUv(refrUv, 0.0)), lighting);
                const float3 b = decodeEnvironment(envAtlasTexture.sample(defaultSampler, mapShinyUv(refrUv, 1.0)), lighting);
                refrSample = mix(a, b, refrLevel);
            } else {
                const float3 a = decodeEnvironment(envAtlasTexture.sample(defaultSampler, mapRoughnessUv(refrUv, refrILevel)), lighting);
                const float3 b = decodeEnvironment(envAtlasTexture.sample(defaultSampler, mapRoughnessUv(refrUv, refrILevel + 1.0)), lighting);
                refrSample = mix(a, b, refrLevel - refrILevel);
            }

            float3 refrColor = processEnvironment(
                refrSample, max(lighting.cameraPositionSkyboxIntensity.w, 0.0));

            // Volume absorption: tint refracted light by base color.
            // Thicker volume = stronger tinting (Beer's law approximation).
            const float absorb = max(material.thickness, 0.0) + 1.0;
            refrColor *= pow(baseLinear, float3(absorb));

            // Fresnel: grazing angles reflect more, normal incidence transmits more.
            // Uses IOR-derived F0 for physically correct Fresnel.
            const float NdotV = max(dot(N, V), 0.0);
            const float F0_ior = pow((1.0 - ior) / (1.0 + ior), 2.0);
            const float fresnel = F0_ior + (1.0 - F0_ior) * pow(1.0 - NdotV, 5.0);
            const float transmission = material.transmissionFactor * (1.0 - fresnel);

            // Blend: replace surface diffuse with refracted view, keep specular.
            const float3 specPart = directSpecular + indirectSpecular + emissiveLinear;
            litLinear = mix(litLinear, refrColor + specPart, transmission);
        }
    }
#endif

#if VT_FEATURE_CLEARCOAT
    // Energy-conserving clearcoat composition.
    // f(v,l) = base * (1 - Fc * ccSpecularity) + (ccSpecularLight + ccReflection) * ccSpecularity
    // The (1 - Fc) factor on the base represents light absorbed by the clearcoat layer:
    // light reflected by the coat cannot also contribute to the base layer response.
    {
        const float ccNdotV = max(dot(ccNormalW, V), 0.0);
        const float ccFresnel = getFresnelCC(ccNdotV);
        const float ccScaling = 1.0 - ccFresnel * ccSpecularity;
        litLinear = litLinear * ccScaling + (ccSpecularLight + ccReflection) * ccSpecularity;
    }
#endif

#if VT_FEATURE_SHEEN
    // Sheen energy conservation: the sheen layer absorbs some energy from the base layer.
    // Factor 0.157 ≈ average directional albedo of the Charlie sheen BRDF,
    // derived from fitting the sheen DFG integral.
    // base *= (1 - max(sheenColor) * 0.157), then add sheen contribution.
    {
        const float sheenScaling = 1.0 - max(sheenTint.r, max(sheenTint.g, sheenTint.b)) * 0.157;
        litLinear = litLinear * sheenScaling + sheenSpecularDirect + sheenSpecularIndirect;
    }
#endif

#if VT_FEATURE_PLANAR_REFLECTION
    // DEVIATION: planar reflection with per-pixel distance-dependent Poisson disk blur.
    // Blurred planar reflection.
    // Upstream implements this as a ShaderMaterial script; here it's a shader feature
    // with blur parameters passed via LightingData.
    //
    // When a depth texture is available (from the depth pass camera), the blur radius
    // scales per-pixel with distance from the reflection plane: objects touching the
    // ground have sharp reflections, objects far from the plane are progressively blurrier.
    // When no depth texture is available, falls back to uniform blur.
    if (reflectionTexture.get_width() > 0) {
        // Screen-space UV from fragment position.
        float2 reflUV = rd.position.xy * lighting.screenInvResolution.xy;
        reflUV.y = 1.0 - reflUV.y;  // Metal: flip Y (top-left origin → bottom-left UV)

        // Unpack blur parameters from LightingData.
        const float reflIntensityParam = lighting.reflectionParams.x;  // 0..1
        const float blurAmount         = lighting.reflectionParams.y;  // 0..2
        const float fadeStrength       = lighting.reflectionParams.z;  // 0..5
        const float angleFade          = lighting.reflectionParams.w;  // 0..1
        const float3 fadeColor         = lighting.reflectionFadeColor.xyz;

        // Per-pixel distance from reflection plane (from depth pass camera).
        // The depth camera outputs abs(worldPos.y + planeDistance) / heightRange.
        float distFromPlane = 0.0;
        bool hasDepthMap = (reflectionDepthTexture.get_width() > 0);
        if (hasDepthMap) {
            distFromPlane = reflectionDepthTexture.sample(defaultSampler, reflUV).r;
        }

        float3 reflColor;
        if (blurAmount > 0.001) {
            // 32-tap Poisson disk blur.
            // Poisson disk offsets distributed in a unit circle for uniform coverage.
            constexpr int NUM_TAPS = 32;
            constexpr float2 poissonTaps[NUM_TAPS] = {
                float2(-0.220147, 0.976896), float2(-0.735514, 0.693436),
                float2(-0.200476, 0.310353), float2( 0.180822, 0.454146),
                float2( 0.292754, 0.937414), float2( 0.564255, 0.207879),
                float2( 0.178031, 0.024583), float2( 0.613912,-0.205936),
                float2(-0.385540,-0.070092), float2( 0.962838, 0.378319),
                float2(-0.886362, 0.032122), float2(-0.466531,-0.741458),
                float2( 0.006773,-0.574796), float2(-0.739828,-0.410584),
                float2( 0.590785,-0.697557), float2(-0.081436,-0.963262),
                float2( 1.000000,-0.100160), float2( 0.622430, 0.680868),
                float2(-0.545396, 0.538133), float2( 0.330651,-0.468300),
                float2(-0.168019,-0.623054), float2( 0.427100, 0.698100),
                float2(-0.827445,-0.304350), float2( 0.765140, 0.556640),
                float2(-0.403340, 0.198600), float2( 0.114050,-0.891450),
                float2(-0.956940, 0.258450), float2( 0.310545,-0.142367),
                float2(-0.143134, 0.619453), float2( 0.870890,-0.227634),
                float2(-0.627623, 0.019867), float2( 0.487623, 0.012367)
            };

            // blur area scales with distance from plane per pixel.
            // area = distFromPlane * 80.0 * blurAmount / texWidth
            // When no depth map is available, use blurAmount directly (uniform blur fallback).
            const float reflTexWidth = float(reflectionTexture.get_width());
            float area;
            if (hasDepthMap) {
                area = distFromPlane * 80.0 * blurAmount / reflTexWidth;
            } else {
                area = blurAmount * 80.0 / reflTexWidth;
            }

            reflColor = float3(0.0);
            for (int i = 0; i < NUM_TAPS; i++) {
                const float2 offset = poissonTaps[i] * area;
                reflColor += reflectionTexture.sample(defaultSampler, reflUV + offset).rgb;
            }
            reflColor /= float(NUM_TAPS);
        } else {
            // Fast path: single sample, no blur.
            reflColor = reflectionTexture.sample(defaultSampler, reflUV).rgb;
        }

        // Apply intensity — fade to fadeColor when intensity is reduced.
        reflColor = mix(fadeColor, reflColor, reflIntensityParam);

        // Fresnel effect based on viewing angle (angleFade).
        // Looking straight down → fade to fadeColor. Grazing angle → full reflection.
        const float NdotV = abs(dot(N, V));
        const float fresnelFade = pow(NdotV, max(angleFade, 0.01));

        // Distance-based fade: approximate by using view distance from camera to fragment.
        const float viewDist = distance(rd.worldPos, cameraPosition);
        const float distanceFade = 1.0 - exp(-viewDist * fadeStrength * 0.1);

        // Combine fades: either distance OR angle can fade to fadeColor.
        const float totalFade = max(distanceFade, fresnelFade);

        // Blend reflection with fade color.
        reflColor = mix(reflColor, fadeColor, totalFade);

        // Final blend: merge blurred reflection into lit surface.
        // Use gloss to modulate — glossier surfaces show more reflection.
        const float blendFactor = gloss * reflIntensityParam;
        litLinear = mix(litLinear, reflColor, blendFactor);
    }
#endif

#if VT_FEATURE_FOG
    if (lighting.fogStartEndType.z > 0.5) {
        const float viewDistance = distance(rd.worldPos, cameraPosition);
        const float fogStart = lighting.fogStartEndType.x;
        const float fogEnd = max(lighting.fogStartEndType.y, fogStart + 1e-3);
        float fogFactor = clamp((fogEnd - viewDistance) / (fogEnd - fogStart), 0.0, 1.0);
        const float density = max(lighting.fogColorDensity.w, 0.0);
        if (density > 0.0) {
            const float expFog = exp(-viewDistance * density);
            fogFactor = min(fogFactor, expFog);
        }
        litLinear = mix(lighting.fogColorDensity.xyz, litLinear, fogFactor);
    }
#endif
    // when CameraFrame is active (bit 5 of flagsAndPad.x),
    // the forward pass outputs linear HDR — tonemapping and gamma are deferred
    // to the compose pass.  This is a runtime check (not a compile-time variant)
    // to avoid doubling the number of compiled shader variants.
    if ((lighting.flagsAndPad.x & (1u << 5)) != 0u) {
        return float4(max(litLinear, float3(0.0)), alpha);
    }
    const float exposure = max(lighting.skyboxMipAndPad.y, 0.0);
    const float tonemapMode = lighting.skyboxMipAndPad.z;
    return float4(linearToSrgb(toneMap(max(litLinear, float3(0.0)), exposure, tonemapMode)), alpha);
#endif
#endif
}
