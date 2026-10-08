// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
    // useLighting off (VT_FEATURE_NO_LIGHTS): no light reaches the surface; ambient,
    // reflections and the combine still run. The clustered loop is compiled out with it.
    // Without VT_FEATURE_MULTI_LIGHT the variant shades one light.
    //
    // Every block below that a variant cannot reach is gated on its FEATURE, not only on a
    // runtime value (a light's shadow slot, its type): specialisation then removes it. Gated
    // at run time alone, the spot, omni and directional shadow code stays in every variant,
    // and the registers it needs cost occupancy where it never runs (Metal post-processing's
    // forward pass read 20% slower than the hand-written MSL, which used #if for these).
    uint count = vtFeatureEnabled(VT_FEATURE_NO_LIGHTS_BIT) ? 0u
        : min(lighting.lightCount.x, vtFeatureEnabled(VT_FEATURE_MULTI_LIGHT_BIT) ? 8u : 1u);
    for (uint i = 0u; i < count; ++i) {
        Light light = lighting.lights[i];
        uint type = uint(light.directionType.w + 0.5);

        // The source's shape (LightShape; 0 punctual): an area light keeps its type's
        // cone, cookie and shadow, takes only the range window as distance falloff, and
        // is shaded with LTC below.
        uint lightShape = vtFeatureEnabled(VT_FEATURE_AREA_LIGHTS_BIT)
            ? uint(light.areaRightHalfWidth.w + 0.5) : 0u;
        float3 L;
        float atten = 1.0;
        // Light cookie: the projected texture masking this light's color. Folded
        // into the radiance below, BEFORE any falloff.
        float3 cookieMask = float3(1.0);
        if (type == 0u) {
            L = normalize(-light.directionType.xyz);
            // A shadowed directional light carries its shadow slot (0 or 1) in
            // coneParams.w; every other directional light carries -1 and stays
            // unshadowed.
            //
            // The slot is passed as a LITERAL, one call per slot, so each inlined copy reads
            // one map and one set of parameters; a runtime slot keeps both slots' lookups
            // and selects in every copy, and that cost Metal's forward pass 15% (the
            // hand-written MSL calls once per slot the same way).
            if (vtFeatureEnabled(VT_FEATURE_SHADOWS_BIT) && light.coneParams.w >= 0.0) {
                if (int(light.coneParams.w + 0.5) == 0) {
                    atten = sampleDirectionalShadow(0, fragWorldPos, fragViewDepth, N, L);
                } else {
                    atten = sampleDirectionalShadow(1, fragWorldPos, fragViewDepth, N, L);
                }
            }
            // Parallax self-shadowing: the height field casts onto itself, which
            // the cascade map cannot see because it only knows the flat polygon.
            // Only the directional light pays for the extra march.
            if (parallaxShadowActive) {
                float3 lightDirTS = normalize(float3(
                    dot(parallaxTangent, L), dot(parallaxBitangent, L),
                    dot(parallaxNormalGeom, L)));
                atten *= parallaxSelfShadow(parallaxUv, lightDirTS,
                    material.heightMapFactor,
                    clamp(material.heightMapParams.x, 0.0, 1.0),
                    parallaxSurfaceDepth, material.heightMapParams.y);
            }
            // Shadow catcher only tracks directional shadows, and accumulates
            // here — before the NdotL early-out below — so back-facing texels
            // still report the shadow they receive.
            if (vtFeatureEnabled(VT_FEATURE_SHADOW_CATCHER_BIT)) {
                dShadowCatcher *= atten;
            }
        } else {
            float3 lightPosition = light.positionRange.xyz;
            float3 toLight = lightPosition - fragWorldPos;
            float dist = length(toLight);
            L = (dist > 1e-4) ? toLight / dist : float3(0.0, 1.0, 0.0);
            // Non-punctual lights only get the range window here — the distance
            // falloff comes from the LTC form factor itself.
            atten = (lightShape != 0u)
                ? ltcFalloffWindow(light.positionRange.w, toLight)
                : distanceAttenuation(dist, light.positionRange.w, light.coneParams.z);

            // A cookie with cookieFalloff disabled replaces the cone falloff
            // entirely — the projection's own clip bounds the beam instead.
            bool cookieReplacesConeFalloff = false;
            if (light.cookieFlags.x > 0.5) {
                int cookieSlot = int(light.cookieFlags.y + 0.5);
                uint cookieChannel = uint(light.cookieFlags.z + 0.5);
                bool cookieFalloff = light.cookieFlags.w > 0.5;
                if (type == 2u && vtFeatureEnabled(VT_FEATURE_COOKIE_2D_BIT)) {
                    cookieMask = getCookie2D(cookieSlot, fragWorldPos, cookieChannel, !cookieFalloff);
                    cookieReplacesConeFalloff = !cookieFalloff;
                } else if (type == 1u && vtFeatureEnabled(VT_FEATURE_COOKIE_CUBE_BIT)) {
                    // The cube is sampled by the light→fragment direction, the
                    // opposite of our L.
                    cookieMask = getCookieCube(cookieSlot, -L, cookieChannel);
                }
            }

            if (type == 2u && !cookieReplacesConeFalloff) {
                float cd = dot(normalize(-light.directionType.xyz), L);
                atten *= getSpotEffect(light.coneParams.x, light.coneParams.y, cd);
            }

            // Local light shadows: coneParams.w carries the shadow slot
            // (-1 = no shadow, 0/1 = local caster).  Point lights use the
            // cubemap path, spot lights the projected 2D path.
            float shadowIndex = light.coneParams.w;
            if (shadowIndex >= 0.0) {
                int slot = int(shadowIndex + 0.5);
                // A literal slot per call, as the directional shadow above.
                if (type == 1u && vtFeatureEnabled(VT_FEATURE_OMNI_SHADOWS_BIT)) {
                    if (slot == 0) {
                        atten *= sampleOmniShadow(0, fragWorldPos, light.positionRange.xyz);
                    } else {
                        atten *= sampleOmniShadow(1, fragWorldPos, light.positionRange.xyz);
                    }
                } else if (type == 2u && vtFeatureEnabled(VT_FEATURE_LOCAL_SHADOWS_BIT)) {
                    const float lightDistance = distance(light.positionRange.xyz, fragWorldPos);
                    if (slot == 0) {
                        atten *= sampleSpotShadow(0, fragWorldPos, N, L, lightDistance);
                    } else {
                        atten *= sampleSpotShadow(1, fragWorldPos, N, L, lightDistance);
                    }
                }
            }
        }

        if (lightShape != 0u) {
            // Area light — LTC, through common-ltc.slang's helpers. The world half
            // axes are the light's scaled X and Z.
            if (atten <= 0.00001) {
                continue;
            }
            float3 lightPos = light.positionRange.xyz;
            float3 p0, p1, p2, p3;
            float sphereRadius = ltcAreaLight(lightShape, lightPos, light.areaRightHalfWidth.xyz,
                light.areaUpHalfHeight.xyz, N, lighting.cameraPosExposure.xyz, p0, p1, p2, p3);
            float3 areaRadiance = light.colorIntensity.rgb * cookieMask * light.colorIntensity.w * atten;
            float3 specFres = ltcSpecularFresnel(N, V, roughness, F0);

            // A directional source keeps plain Lambert; a local one integrates its shape.
            float ltcDiffuse = (type == 0u)
                ? max(dot(N, L), 0.0)
                : ltcAreaDiffuse(lightShape, sphereRadius, lightPos, N, V, fragWorldPos, p0, p1, p2, p3);
            float3 areaDiffuse = diffuseAlbedo * areaRadiance * ltcDiffuse * (float3(1.0) - specFres);
            color += areaDiffuse;
            directDiffuse += areaDiffuse;

            float3 areaSpecular = areaRadiance * specFres * specularOn *
                ltcAreaSpecular(lightShape, N, V, fragWorldPos, roughness, p0, p1, p2, p3);
            color += areaSpecular;
            directSpecular += areaSpecular;

            if (vtFeatureEnabled(VT_FEATURE_CLEARCOAT_BIT)) {
                // Clearcoat LTC specular with F0 = 0.04, at the coat's own normal and
                // gloss; accumulated, the tail applies ccSpecularity.
                ccSpecularLight += areaRadiance *
                    ltcSpecularFresnel(ccNormalW, V, 1.0 - ccGlossiness, float3(0.04)) *
                    ltcAreaSpecular(lightShape, ccNormalW, V, fragWorldPos, 1.0 - ccGlossiness, p0, p1, p2, p3);
            }
            continue;
        }

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0 || atten <= 0.0) {
            continue;
        }

        float3 H = normalize(L + V);
        float NdotH = max(dot(N, H), 0.0);
        float VdotH = max(dot(V, H), 0.0);

        float D = distributionGGX(NdotH, roughness);
        // Height-correlated visibility folds in 1/(4 NdotL NdotV), so there is no
        // explicit division here — see common-brdf.slang.
        float Vis = getVisibilitySmithGGX(NdotV, NdotL, roughness);
        if (vtFeatureEnabled(VT_FEATURE_ANISOTROPY_BIT)) {
            // Anisotropic GGX (common-brdf): D carries the whole D * Vis product.
            D = getLightSpecularAnisoGGX(N, V, H, L, anisoT, anisoB, anisoAlpha);
            Vis = 1.0;
        }
        // Directional lights take the gloss-aware Fresnel; punctual lights take bare
        // specularity. Applying the Fresnel to every light type over-brightened the
        // rim of everything lit by a point or spot light.
        float3 F = (type == 0u) ? getFresnel(VdotH, 1.0 - roughness, F0) : F0;
        if (vtFeatureEnabled(VT_FEATURE_IRIDESCENCE_BIT)) {
            F = lerp(F, iridFresnel, iridIntensity);
        }

        float3 specular = D * Vis * F;

        float3 radiance = light.colorIntensity.rgb * cookieMask * light.colorIntensity.w * atten;
        // Oren-Nayar rough diffuse (fast qualitative form): retro-reflection for
        // rough surfaces instead of plain Lambert.
        float diffuseTerm = 1.0;
        if (vtFeatureEnabled(VT_FEATURE_OREN_NAYAR_BIT)) {
            float sigma2 = roughness * roughness;
            float onA = 1.0 - 0.5 * sigma2 / (sigma2 + 0.33);
            float onB = 0.45 * sigma2 / (sigma2 + 0.09);
            float sTerm = dot(L, V) - NdotL * NdotV;
            float tTerm = sTerm <= 0.0 ? 1.0 : max(max(NdotL, NdotV), 1e-4);
            diffuseTerm = onA + onB * sTerm / tTerm;
        }
        // Direct diffuse is albedo * radiance * NdotL — no 1/PI and no
        // energy-conservation factor: the Lambert term is a bare NdotL and the
        // combine multiplies by albedo. Dividing by PI and multiplying by
        // kD = (1 - F)(1 - metallic) would make every direct light about a third as
        // bright, and kD would apply (1 - metallic) a second time, since
        // diffuseAlbedo carries it.
        // With area lights in the variant, a punctual light's diffuse is
        // scaled by (1 - specularity) (the LTC lights take (1 - their Fresnel)).
        float3 punctualDiffuseScale = (vtFeatureEnabled(VT_FEATURE_AREA_LIGHTS_BIT) && specularOn > 0.0)
            ? float3(1.0) - F0 : float3(1.0);
        color += (diffuseAlbedo * diffuseTerm * punctualDiffuseScale + specular * specularOn) * radiance * NdotL;
        directDiffuse += diffuseAlbedo * diffuseTerm * punctualDiffuseScale * radiance * NdotL;
        bakeDiffuseLight += diffuseTerm * radiance * NdotL;
        bakeDirectLight += diffuseTerm * radiance * NdotL;
        directSpecular += specular * specularOn * radiance * NdotL;
        if (vtFeatureEnabled(VT_FEATURE_CLEARCOAT_BIT)) {
            // Clearcoat: GGX distribution, Kelemen visibility (a coat is smooth
            // enough that Smith-GGX is not worth its cost) and a fixed F0 = 0.04
            // Fresnel, all taken at the clearcoat's own half vector, and weighted by
            // NdotL (a division by 4*NdotV with no NdotL is not a reflectance
            // integral: the coat would brighten as the surface turned away from the
            // light).
            // Taken at the COAT normal with the coat's own roughness.
            float ccNdotL = max(dot(ccNormalW, L), 0.0);
            float ccNdotH = max(dot(ccNormalW, H), 0.0);
            float ccLdotH = max(dot(L, H), 0.0);
            float ccDenom = ccNdotH * ccNdotH * (ccAlpha2 - 1.0) + 1.0;
            float ccD = ccAlpha2 / max(PI * ccDenom * ccDenom, 1e-7);
            float ccVis = getVisibilityKelemen(ccLdotH);
            float ccF = getFresnelCC(ccLdotH);
            // Accumulated; the tail applies ccSpecularity and dims the base.
            ccSpecularLight += radiance * ccNdotL * ccD * ccVis * ccF;
        }
        if (vtFeatureEnabled(VT_FEATURE_SHEEN_BIT)) {
            // Charlie distribution + Ashikhmin visibility (common-sheen.slang),
            // accumulated apart so the tail can take its energy out of the base
            // layer. Was an ad-hoc velvet power added straight into color.
            sheenSpecularDirect += radiance * NdotL * sheenTint
                * sheenDistribution(NdotH, sheenRoughness)
                * sheenVisibility(NdotV, NdotL);
        }
    }

