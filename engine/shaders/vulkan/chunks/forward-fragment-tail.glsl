
    // ── Blurred planar reflection (parity with forward-fragment-tail.metal) ──
    // DEVIATION (inherited from the Metal chunk): upstream implements planar
    // reflection as a ShaderMaterial script; here it is a shader feature fed by
    // LightingData. The reflection camera renders the mirrored scene into
    // binding 15; the optional depth camera writes a per-pixel
    // distance-from-plane into binding 16, which scales the blur radius so
    // objects touching the ground reflect sharply and distant ones smear.
    if (vtFeatureEnabled(VT_FEATURE_PLANAR_REFLECTION_BIT) &&
        lighting.reflectionDepthParams.z > 0.5) {
        // Screen-space UV. gl_FragCoord and the sampler UV are both top-left
        // origin here, and the reflection target was rendered through the same
        // negative-height viewport as the main pass, so this matches the Metal
        // path texel for texel — including its Y flip, which mirrors the image
        // rather than correcting any coordinate convention.
        vec2 reflUV = gl_FragCoord.xy * lighting.screenInvResolution.xy;
        reflUV.y = 1.0 - reflUV.y;

        float reflIntensityParam = lighting.reflectionParams.x;
        float blurAmount         = lighting.reflectionParams.y;
        float fadeStrength       = lighting.reflectionParams.z;
        float angleFade          = lighting.reflectionParams.w;
        vec3  fadeColor          = lighting.reflectionFadeColor.xyz;

        // Metal gates the depth map on get_width(); an unbound separate image
        // samples as white here, so the flag comes from the uniform instead.
        bool hasDepthMap = lighting.reflectionDepthParams.w > 0.5;
        float distFromPlane = hasDepthMap
            ? texture(planarReflectionDepth, reflUV).r : 0.0;

        vec3 reflColor;
        if (blurAmount > 0.001) {
            // 32-tap Poisson disk, offsets distributed in a unit circle.
            const int NUM_TAPS = 32;
            const vec2 poissonTaps[NUM_TAPS] = vec2[NUM_TAPS](
                vec2(-0.220147, 0.976896), vec2(-0.735514, 0.693436),
                vec2(-0.200476, 0.310353), vec2( 0.180822, 0.454146),
                vec2( 0.292754, 0.937414), vec2( 0.564255, 0.207879),
                vec2( 0.178031, 0.024583), vec2( 0.613912,-0.205936),
                vec2(-0.385540,-0.070092), vec2( 0.962838, 0.378319),
                vec2(-0.886362, 0.032122), vec2(-0.466531,-0.741458),
                vec2( 0.006773,-0.574796), vec2(-0.739828,-0.410584),
                vec2( 0.590785,-0.697557), vec2(-0.081436,-0.963262),
                vec2( 1.000000,-0.100160), vec2( 0.622430, 0.680868),
                vec2(-0.545396, 0.538133), vec2( 0.330651,-0.468300),
                vec2(-0.168019,-0.623054), vec2( 0.427100, 0.698100),
                vec2(-0.827445,-0.304350), vec2( 0.765140, 0.556640),
                vec2(-0.403340, 0.198600), vec2( 0.114050,-0.891450),
                vec2(-0.956940, 0.258450), vec2( 0.310545,-0.142367),
                vec2(-0.143134, 0.619453), vec2( 0.870890,-0.227634),
                vec2(-0.627623, 0.019867), vec2( 0.487623, 0.012367)
            );

            float reflTexWidth = float(textureSize(planarReflection, 0).x);
            float area = hasDepthMap
                ? distFromPlane * 80.0 * blurAmount / reflTexWidth
                : blurAmount * 80.0 / reflTexWidth;

            reflColor = vec3(0.0);
            for (int i = 0; i < NUM_TAPS; ++i) {
                reflColor += texture(planarReflection,
                    reflUV + poissonTaps[i] * area).rgb;
            }
            reflColor /= float(NUM_TAPS);
        } else {
            reflColor = texture(planarReflection, reflUV).rgb;
        }

        // Intensity fades toward fadeColor.
        reflColor = mix(fadeColor, reflColor, reflIntensityParam);

        // Fresnel fade: straight down → fadeColor, grazing → full reflection.
        float NdotVAbs = abs(dot(N, V));
        float fresnelFade = pow(NdotVAbs, max(angleFade, 0.01));

        // Distance fade, approximated from the camera-to-fragment distance.
        float viewDist = distance(fragWorldPos, lighting.cameraPosExposure.xyz);
        float distanceFade = 1.0 - exp(-viewDist * fadeStrength * 0.1);

        // Either fade can pull the result toward fadeColor.
        reflColor = mix(reflColor, fadeColor, max(distanceFade, fresnelFade));

        // Glossier surfaces show more reflection.
        float reflGloss = 1.0 - roughness;
        color = mix(color, reflColor, reflGloss * reflIntensityParam);
    }

    // Debug surface-quantity output (upstream debug-output.js). Replaces the
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
                outColor = vec4(pow(max(albedo.rgb, vec3(0.0)) + 0.0000001,
                    vec3(1.0 / 2.2)), 1.0);
                return;
            } else if (debugPass == VT_DEBUGPASS_WORLDNORMAL) {
                outColor = vec4(N * 0.5 + 0.5, 1.0);
                return;
            } else if (debugPass == VT_DEBUGPASS_OPACITY) {
                outColor = vec4(vec3(albedo.a), 1.0);
                return;
            } else if (debugPass == VT_DEBUGPASS_SPECULARITY) {
                outColor = vec4(F0, 1.0);
                return;
            } else if (debugPass == VT_DEBUGPASS_GLOSS) {
                outColor = vec4(vec3(1.0 - roughness), 1.0);
                return;
            } else if (debugPass == VT_DEBUGPASS_METALNESS) {
                outColor = vec4(vec3(metallic), 1.0);
                return;
            } else if (debugPass == VT_DEBUGPASS_AO) {
                outColor = vec4(vec3(ao), 1.0);
                return;
            } else if (debugPass == VT_DEBUGPASS_EMISSION) {
                outColor = vec4(pow(max(emissive, vec3(0.0)) + 0.0000001,
                    vec3(1.0 / 2.2)), 1.0);
                return;
            } else if (debugPass == VT_DEBUGPASS_UV0) {
                outColor = vec4(fragUV0, 0.0, 1.0);
                return;
            }
        }
    }

    // Fog (linear or exponential) toward the fog color.
    float fogType = vtFeatureEnabled(VT_FEATURE_FOG_BIT)
        ? lighting.fogStartEndType.z : 0.0;
    if (fogType > 0.5) {
        float dist = length(lighting.cameraPosExposure.xyz - fragWorldPos);
        float f;
        if (fogType < 1.5) {
            f = clamp((lighting.fogStartEndType.y - dist) /
                      max(lighting.fogStartEndType.y - lighting.fogStartEndType.x, 1e-4), 0.0, 1.0);
        } else {
            float d = dist * lighting.fogColorDensity.w;
            f = clamp(exp(-d * d), 0.0, 1.0);
        }
        color = mix(lighting.fogColorDensity.rgb, color, f);
    }

    if (vtFeatureEnabled(VT_FEATURE_LIGHTMAP_BAKE_BIT)) {
        // Bake output: light only, sRGB-encoded for the RGBA8 target and the pow(2.2)
        // decode on the sampling side. No exposure or tonemap — the lightmap feeds the
        // lit path, which applies both later.
        // Linear, so the accumulating virtual-light passes sum correctly (see the Metal chunk).
        // Accumulating passes add only their own light — the first pass wrote the ambient.
        vec3 baked = vtFeatureEnabled(VT_FEATURE_LIGHTMAP_BAKE_ACCUM_BIT)
            ? bakeDirectLight : bakeDiffuseLight;
        outColor = vec4(max(baked, vec3(0.0)), 1.0);
        return;
    }

    // Exposure, tonemap, then display-gamma encode (swapchain is a linear
    // UNORM target, matching the Metal BGRA8Unorm drawable).
    color *= lighting.cameraPosExposure.w;
    color = applyToneMap(color);
    color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));

    outColor = vec4(color, albedo.a);
}
