// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//

    // The layers stack from the inside out: the sheen sits on the base, the
    // clearcoat on top of both, so the coat's Fresnel also dims the sheen. Twin of
    // forward-fragment-tail.metal, which also blends the planar reflection after them.

    // Sheen energy conservation: the sheen layer takes energy from the base layer
    // rather than adding on top of it. 0.157 is the average directional albedo of
    // the Charlie BRDF, from fitting its DFG integral. Twin of the block in
    // forward-fragment-tail.metal.
    if (vtFeatureEnabled(VT_FEATURE_SHEEN_BIT)) {
        float sheenScaling = 1.0 -
            max(sheenTint.r, max(sheenTint.g, sheenTint.b)) * 0.157;
        color = color * sheenScaling + sheenSpecularDirect + sheenSpecularIndirect;
    }

    if (vtFeatureEnabled(VT_FEATURE_CLEARCOAT_BIT)) {
        // Energy-conserving clearcoat composition, twin of forward-fragment-tail.metal:
        //   f = base * (1 - Fc * ccSpecularity) + (ccSpecularLight + ccReflection) * ccSpecularity
        // Light the coat reflects cannot also reach the base, so the base is dimmed
        // by the coat's Fresnel; the coat's own direct and environment specular are
        // added scaled by the coat factor.
        float ccNdotV = max(dot(ccNormalW, V), 0.0);
        float ccScaling = 1.0 - getFresnelCC(ccNdotV) * ccSpecularity;
        color = color * ccScaling + (ccSpecularLight + ccReflection) * ccSpecularity;
    }

    // Emission is added after the layers are combined: an emissive surface under a
    // coat or sheen keeps its full emission.
    color += emissive;

    // ── Blurred planar reflection (parity with forward-fragment-tail.metal) ──
    // DEVIATION (inherited from the Metal chunk): upstream implements planar
    // reflection as a ShaderMaterial script; here it is a shader feature fed by
    // LightingData. The reflection camera renders the mirrored scene into
    // binding 15; the optional depth camera writes a per-pixel
    // distance-from-plane into binding 16, which scales the blur radius so
    // objects touching the ground reflect sharply and distant ones smear.
    if (vtFeatureEnabled(VT_FEATURE_PLANAR_REFLECTION_BIT) &&
        lighting.reflectionDepthParams.z > 0.5) {
        // Screen-space UV. vtFragCoord and the sampler UV are both top-left
        // origin here, and the reflection target was rendered through the same
        // negative-height viewport as the main pass, so this matches the Metal
        // path texel for texel — including its Y flip, which mirrors the image
        // rather than correcting any coordinate convention.
        float2 reflUV = vtFragCoord.xy * lighting.screenInvResolution.xy;
        reflUV.y = 1.0 - reflUV.y;

        float reflIntensityParam = lighting.reflectionParams.x;
        float blurAmount         = lighting.reflectionParams.y;
        float fadeStrength       = lighting.reflectionParams.z;
        float angleFade          = lighting.reflectionParams.w;
        float3  fadeColor          = lighting.reflectionFadeColor.xyz;

        // Metal gates the depth map on get_width(); an unbound separate image
        // samples as white here, so the flag comes from the uniform instead.
        bool hasDepthMap = lighting.reflectionDepthParams.w > 0.5;
        float distFromPlane = hasDepthMap
            ? planarReflectionDepth.Sample(linearClampSampler, reflUV).r : 0.0;

        float3 reflColor;
        if (blurAmount > 0.001) {
            // 32-tap Poisson disk, offsets distributed in a unit circle.
            const int NUM_TAPS = 32;
            const float2 poissonTaps[NUM_TAPS] = float2[NUM_TAPS](
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
            );

            float reflTexWidth = float(vtTextureSize(planarReflection, 0).x);
            float area = hasDepthMap
                ? distFromPlane * 80.0 * blurAmount / reflTexWidth
                : blurAmount * 80.0 / reflTexWidth;

            reflColor = float3(0.0);
            for (int i = 0; i < NUM_TAPS; ++i) {
                reflColor += planarReflection.Sample(linearClampSampler, reflUV + poissonTaps[i] * area).rgb;
            }
            reflColor /= float(NUM_TAPS);
        } else {
            reflColor = planarReflection.Sample(linearClampSampler, reflUV).rgb;
        }

        // Intensity fades toward fadeColor.
        reflColor = lerp(fadeColor, reflColor, reflIntensityParam);

        // Fresnel fade: straight down → fadeColor, grazing → full reflection.
        float NdotVAbs = abs(dot(N, V));
        float fresnelFade = pow(NdotVAbs, max(angleFade, 0.01));

        // Distance fade, approximated from the camera-to-fragment distance.
        float viewDist = distance(fragWorldPos, lighting.cameraPosExposure.xyz);
        float distanceFade = 1.0 - exp(-viewDist * fadeStrength * 0.1);

        // Either fade can pull the result toward fadeColor.
        reflColor = lerp(reflColor, fadeColor, max(distanceFade, fresnelFade));

        // Glossier surfaces show more reflection.
        float reflGloss = 1.0 - roughness;
        color = lerp(color, reflColor, reflGloss * reflIntensityParam);
    }

    // Debug surface-quantity output. Replaces the
    // shaded result with one input to the lighting equation. DEBUGPASS_LIGHTING
    // is deliberately absent here — it is handled above by neutralizing albedo,
    // then falls through so it still gets fog and tonemapping.
    //
    // Colour quantities (albedo, emission) are gamma encoded to match how they
    // would be displayed; the rest are written raw, as they are already display
    // values. NOTE: when the camera runs a CameraFrame pass, compose applies
    // tonemapping and gamma on top of whatever is returned here, so debug values
    // only read back exactly with postprocessing off.
    if (vtFeatureEnabled(VT_FEATURE_DEBUG_PASS_BIT)) {
        uint debugPass = lighting.flagsAndPad[1];
        if (debugPass != VT_DEBUGPASS_NONE && debugPass != VT_DEBUGPASS_LIGHTING) {
            if (debugPass == VT_DEBUGPASS_ALBEDO) {
                outColor = float4(pow(max(albedo.rgb, float3(0.0)) + 0.0000001,
                    float3(1.0 / 2.2)), 1.0);
                return outColor;
            } else if (debugPass == VT_DEBUGPASS_WORLDNORMAL) {
                outColor = float4(N * 0.5 + 0.5, 1.0);
                return outColor;
            } else if (debugPass == VT_DEBUGPASS_OPACITY) {
                outColor = float4(float3(albedo.a), 1.0);
                return outColor;
            } else if (debugPass == VT_DEBUGPASS_SPECULARITY) {
                outColor = float4(F0, 1.0);
                return outColor;
            } else if (debugPass == VT_DEBUGPASS_GLOSS) {
                outColor = float4(float3(1.0 - roughness), 1.0);
                return outColor;
            } else if (debugPass == VT_DEBUGPASS_METALNESS) {
                outColor = float4(float3(metallic), 1.0);
                return outColor;
            } else if (debugPass == VT_DEBUGPASS_AO) {
                outColor = float4(float3(ao), 1.0);
                return outColor;
            } else if (debugPass == VT_DEBUGPASS_EMISSION) {
                outColor = float4(pow(max(emissive, float3(0.0)) + 0.0000001,
                    float3(1.0 / 2.2)), 1.0);
                return outColor;
            } else if (debugPass == VT_DEBUGPASS_UV0) {
                outColor = float4(fragUV0, 0.0, 1.0);
                return outColor;
            }
        }
    }

    // Fog. The three curves: LINEAR over [start, end], EXP
    // on density, EXP2 on density squared, chosen by the type uploaded in
    // fogStartEndType.z (0 is off).
    //
    // DEVIATION: depth is the LINEAR view-space depth (clip.w), where upstream uses
    // vtFragCoord.z / vtFragCoord.w. That quantity is an old GL convenience: it
    // reaches 0 at the near plane rather than the near distance. Both backends here
    // take clip.w so they agree exactly and the falloff is metric. It is NOT the
    // radial distance to the camera: at a wide field of view that would fog the edges
    // of the frame harder than the centre, by 1/cos(fov/2).
    //
    // fragViewDepth is gl_Position.w from the vertex stage — see forward.vert.
    uint fogType = vtFeatureEnabled(VT_FEATURE_FOG_BIT)
        ? uint(lighting.fogStartEndType.z + 0.5) : 0u;
    if (fogType != 0u) {
        float depth = fragViewDepth;
        float density = max(lighting.fogColorDensity.w, 0.0);
        float f;
        if (fogType == 1u) {
            float fogStart = lighting.fogStartEndType.x;
            float fogEnd = max(lighting.fogStartEndType.y, fogStart + 1e-3);
            f = (fogEnd - depth) / (fogEnd - fogStart);
        } else if (fogType == 2u) {
            f = exp(-depth * density);
        } else {
            float d = depth * density;
            f = exp(-d * d);
        }
        color = lerp(lighting.fogColorDensity.rgb, color, clamp(f, 0.0, 1.0));
    }

    if (vtFeatureEnabled(VT_FEATURE_LIGHTMAP_BAKE_BIT)) {
        // Bake output: light only, sRGB-encoded for the RGBA8 target and the pow(2.2)
        // decode on the sampling side. No exposure or tonemap — the lightmap feeds the
        // lit path, which applies both later.
        // Linear, so the accumulating virtual-light passes sum correctly (see the Metal chunk).
        // Accumulating passes add only their own light — the first pass wrote the ambient.
        float3 baked = vtFeatureEnabled(VT_FEATURE_LIGHTMAP_BAKE_ACCUM_BIT)
            ? bakeDirectLight : bakeDiffuseLight;
        outColor = float4(max(baked, float3(0.0)), 1.0);
        return outColor;
    }

    // When CameraFrame is active (bit 5 of flagsAndPad[0]) the forward pass
    // outputs linear HDR and the compose pass applies exposure, tonemapping and
    // gamma. Runtime check, not a shader variant — same as the Metal chunk.
    if ((lighting.flagsAndPad[0] & (1u << 5)) != 0u) {
        outColor = float4(max(color, float3(0.0)), albedo.a);
        return outColor;
    }

    // Exposure, tonemap, then display-gamma encode (swapchain is a linear
    // UNORM target, matching the Metal BGRA8Unorm drawable).
    color = applyToneMap(color);   // exposure + tonemap
    color = pow(max(color, float3(0.0)), float3(1.0 / 2.2));

    outColor = float4(color, albedo.a);
    return outColor;
}
