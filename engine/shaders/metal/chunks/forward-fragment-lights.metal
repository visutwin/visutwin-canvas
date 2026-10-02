// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
    for (uint i = 0u; i < loopLightCount; ++i) {
        const GpuLight light = lighting.lights[i];
        const uint lightType = light.typeCastShadows.x;
        const bool lightCastsShadows = (light.typeCastShadows.y != 0u);
        const bool falloffModeLinear = (light.typeCastShadows.z != 0u);
        const float3 lightColor = max(light.colorIntensity.xyz, float3(0.0));
        const float lightIntensity = max(light.colorIntensity.w, 0.0);
        if (lightIntensity <= 0.0) {
            continue;
        }

        // The source's shape (LightShape; 0 punctual): an area light keeps its type's
        // cone, cookie and shadow, takes only the range window as distance falloff, and
        // is shaded with LTC below.
        const uint lightShape = uint(light.areaHalfWidth.w + 0.5);
        float3 L = float3(0.0, 1.0, 0.0);
        float attenuation = 1.0;
        float3 lightDirW = float3(0.0);
        // Light cookie: a projected texture that
        // masks the light color. Multiplied into the radiance below, BEFORE any
        // falloff — the two are independent.
        float3 cookieMask = float3(1.0);
        // A cookie with cookieFalloff disabled replaces the cone falloff entirely:
        // the projection's own clip bounds the beam instead (getSpotEffect is
        // skipped in exactly that case).
        bool cookieReplacesConeFalloff = false;
        if (lightType == 0u) {
            const float3 lightDir = light.directionCone.xyz;
            if (length_squared(lightDir) <= 1e-8) {
                continue;
            }
            L = -normalize(lightDir);
        }
        else {
            lightDirW = light.positionRange.xyz - rd.worldPos;
            const float lightDirLenSq = dot(lightDirW, lightDirW);
            if (lightDirLenSq <= 1e-8) {
                continue;
            }
            const float invLightDirLen = rsqrt(lightDirLenSq);
            const float3 dLightDirNormW = lightDirW * invLightDirLen;
            L = dLightDirNormW;

#if VT_FEATURE_COOKIE_2D || VT_FEATURE_COOKIE_CUBE
            if (light.cookieFlags.x != 0u) {
                const uint cookieIdx = light.cookieFlags.y;
                const uint cookieChannel = light.cookieFlags.z;
                const bool cookieFalloff = (light.cookieFlags.w != 0u);
#if VT_FEATURE_COOKIE_2D
                if (lightType == 2u) {
                    const float4x4 cookieXform = (cookieIdx == 0u)
                        ? lighting.cookieMatrix2D0 : lighting.cookieMatrix2D1;
                    const float4 cookieParams = (cookieIdx == 0u)
                        ? lighting.cookieParams2D0 : lighting.cookieParams2D1;
                    cookieMask = (cookieIdx == 0u)
                        ? getCookie2D(cookieTexture2D0, cookieXform, rd.worldPos,
                                      cookieParams.x, cookieChannel, !cookieFalloff,
                                      lighting.cookieTransform2D[0])
                        : getCookie2D(cookieTexture2D1, cookieXform, rd.worldPos,
                                      cookieParams.x, cookieChannel, !cookieFalloff,
                                      lighting.cookieTransform2D[1]);
                    cookieReplacesConeFalloff = !cookieFalloff;
                }
#endif
#if VT_FEATURE_COOKIE_CUBE
                if (lightType == 1u) {
                    const float4x4 cookieXform = (cookieIdx == 0u)
                        ? lighting.cookieMatrixCube0 : lighting.cookieMatrixCube1;
                    const float4 cookieParams = (cookieIdx == 0u)
                        ? lighting.cookieParamsCube0 : lighting.cookieParamsCube1;
                    // The cube is sampled by the direction from the light to the
                    // fragment — the opposite of our L.
                    cookieMask = (cookieIdx == 0u)
                        ? getCookieCube(cookieTextureCube0, cookieXform, -dLightDirNormW,
                                        cookieParams.x, cookieChannel)
                        : getCookieCube(cookieTextureCube1, cookieXform, -dLightDirNormW,
                                        cookieParams.x, cookieChannel);
                }
#endif
            }
#endif

#if VT_FEATURE_POINT_SPOT_ATTENUATION
            if (lightShape != 0u) {
                // Non-punctual lights only get the range window here — the distance
                // falloff comes from the LTC form factor itself.
                attenuation = getFalloffWindow(light.positionRange.w, lightDirW);
            } else if (falloffModeLinear) {
                attenuation = getFalloffLinear(light.positionRange.w, lightDirW);
            } else {
                attenuation = getFalloffInvSquared(light.positionRange.w, lightDirW);
            }

            if (lightType == 2u && !cookieReplacesConeFalloff) {
                const float3 spotDir = normalize(light.directionCone.xyz);
                const float outerConeCos = clamp(light.coneAngles.y, -1.0, 1.0);
                const float innerConeCos = clamp(light.coneAngles.x, outerConeCos, 1.0);
                attenuation *= getSpotEffect(spotDir, innerConeCos, outerConeCos, -dLightDirNormW);
            }
#endif
        }

        if (attenuation < 0.00001) {
            continue;
        }

        float shadowFactor = 1.0;

#if VT_FEATURE_LOCAL_SHADOWS
        // Local light shadow sampling (spot lights with 2D depth texture).
        // Each shadow-casting local light has a VP matrix and depth texture bound at slot 11 or 12.
        if (lightCastsShadows && lightType != 0u
#if VT_FEATURE_OMNI_SHADOWS
            && lightType != 1u  // Omni lights handled by cubemap path below
#endif
        ) {
            const uint shadowIdx = light.typeCastShadows.w;
            const float4x4 shadowMatrix = (shadowIdx == 0u) ? lighting.localShadowMatrix0 : lighting.localShadowMatrix1;
            const float4 shadowParamsLocal = (shadowIdx == 0u) ? lighting.localShadowParams0 : lighting.localShadowParams1;
            const float4 pcssLocal = (shadowIdx == 0u) ? lighting.localShadowPcss0 : lighting.localShadowPcss1;
            // A VSM spot (pcss.w): no normal offset.
            const bool localVsm = pcssLocal.w > 0.5;

            // Apply normal bias in world space, scaled by sin(angle) between
            // normal and light direction so grazing surfaces get more offset.
            const float localNdotL = saturate(dot(N, L));
            const float localSinAngle = sqrt(1.0 - localNdotL * localNdotL);
            const float3 biasedPos = localVsm ? rd.worldPos
                : rd.worldPos + N * (shadowParamsLocal.y * localSinAngle);
            const float4 shadowClip = shadowMatrix * float4(biasedPos, 1.0);
            const float shadowW = max(shadowClip.w, 1e-6);
            const float3 shadowCoord = shadowClip.xyz / shadowW;

            if (shadowCoord.x >= 0.0 && shadowCoord.x <= 1.0 &&
                shadowCoord.y >= 0.0 && shadowCoord.y <= 1.0 &&
                shadowCoord.z >= 0.0 && shadowCoord.z <= 1.0) {

                const float receiverDepth = shadowCoord.z - shadowParamsLocal.x;
                // PCSS (SHADOW_PCSS_32F on the light): contact-hardening soft
                // shadows — runtime uniform branch, no extra shader variant.
                float visible = 1.0;
                if (localVsm) {
                    // The receiver is its distance over the
                    // range (pcss.z, the shadow camera's far), less the 0.0002 bias, and
                    // params.y is the Chebyshev variance bias.
                    const float receiverRatio = length(lightDirW) / pcssLocal.z - shadowParamsLocal.x;
                    visible = (shadowIdx == 0u)
                        ? getShadowVSM16(localVsmTexture0, shadowCoord.xy, receiverRatio, shadowParamsLocal.y)
                        : getShadowVSM16(localVsmTexture1, shadowCoord.xy, receiverRatio, shadowParamsLocal.y);
                } else if (shadowIdx == 0u) {
                    const float res0 = float(localShadowTexture0.get_width());
                    if (res0 > 0.0) {
                        visible = (pcssLocal.x > 0.0)
                            ? getShadowPCSSSpot(localShadowTexture0,
                                float3(shadowCoord.xy, receiverDepth),
                                pcssLocal.x, pcssLocal.y, pcssLocal.z, rd.position.xy)
                            : getShadowPCF3x3(localShadowTexture0, shadowCoord.xy, receiverDepth, res0);
                    }
                } else {
                    const float res1 = float(localShadowTexture1.get_width());
                    if (res1 > 0.0) {
                        visible = (pcssLocal.x > 0.0)
                            ? getShadowPCSSSpot(localShadowTexture1,
                                float3(shadowCoord.xy, receiverDepth),
                                pcssLocal.x, pcssLocal.y, pcssLocal.z, rd.position.xy)
                            : getShadowPCF3x3(localShadowTexture1, shadowCoord.xy, receiverDepth, res1);
                    }
                }
                shadowFactor = mix(1.0 - clamp(shadowParamsLocal.z, 0.0, 1.0), 1.0, visible);
            }
        }
#endif

#if VT_FEATURE_OMNI_SHADOWS
        // Omni (point) light cubemap shadow sampling.
        // Direction from light to fragment selects the cubemap face; perspective-mapped
        // depth comparison determines visibility.
        if (lightCastsShadows && lightType == 1u) {
            const uint shadowIdx = light.typeCastShadows.w;
            const float4 omniParams = (shadowIdx == 0u) ? lighting.omniShadowParams0 : lighting.omniShadowParams1;
            const float4 omniExtra = (shadowIdx == 0u) ? lighting.omniShadowParams0Extra : lighting.omniShadowParams1Extra;

            const float near_val = omniParams.x;
            const float far_val = omniParams.y;
            const float bias = omniParams.z;
            const float intensity = omniExtra.x;

            // Direction from light to fragment (world space) — used for cubemap face selection.
            const float3 lightToFrag = rd.worldPos - light.positionRange.xyz;

            // Eye-space depth for the dominant cubemap face = max(|x|, |y|, |z|).
            const float3 absDir = abs(lightToFrag);
            const float d = max(absDir.x, max(absDir.y, absDir.z));

            // Perspective-mapped depth matching the shadow vertex shader's output:
            // The frustum matrix uses OpenGL convention (z_ndc in [-1,1]),
            // shadow vertex shader remaps: clip.z = 0.5 * (clip.z + clip.w) → [0,1].
            // Resulting stored depth = far * (d - near) / ((far - near) * d).
            //
            // The bias is RELATIVE (a fraction of the distance), applied before the
            // projection rather than as an offset after it. Perspective depth is
            // crushed against 1.0 out here — with near 0.01 and far 30, half a world
            // unit of separation is 8e-5 of stored depth, so any fixed offset large
            // enough to stop acne also swallows every real shadow. Scaling the
            // distance keeps the bias proportionate at any range.
            const float dBiased = d * (1.0 - bias);
            const float denom = (far_val - near_val) * dBiased;
            const float compareValue = far_val * (dBiased - near_val) / max(denom, 1e-6);

            constexpr sampler omniShadowSampler(coord::normalized, filter::linear,
                                                compare_func::less_equal, address::clamp_to_edge);

            // PCSS (SHADOW_PCSS_32F on the light): Vogel-sphere contact-hardening
            // cubemap shadows — runtime uniform branch, no extra shader variant.
            const float4 pcssOmni = (shadowIdx == 0u) ? lighting.localShadowPcss0 : lighting.localShadowPcss1;
            float visible = 1.0;
            if (pcssOmni.x > 0.0) {
                visible = (shadowIdx == 0u)
                    ? getShadowPCSSOmni(omniShadowCube0, lightToFrag,
                        pcssOmni.x, pcssOmni.y, pcssOmni.z, bias, rd.position.xy)
                    : getShadowPCSSOmni(omniShadowCube1, lightToFrag,
                        pcssOmni.x, pcssOmni.y, pcssOmni.z, bias, rd.position.xy);
            } else if (shadowIdx == 0u) {
                visible = omniShadowCube0.sample_compare(omniShadowSampler, lightToFrag, compareValue, level(0));
            } else {
                visible = omniShadowCube1.sample_compare(omniShadowSampler, lightToFrag, compareValue, level(0));
            }

            shadowFactor = mix(1.0 - clamp(intensity, 0.0, 1.0), 1.0, visible);
        }
#endif

#if VT_FEATURE_SHADOWS
        // Directional shadow: the light's shadow index picks its slot (0 or 1).
        // Gated on the slot's flag, never on the texture — an unbound Metal
        // texture still reports a width and samples zero.
        if (lightType == 0u && lightCastsShadows) {
            // CSM: the cascade comes from the fragment's linear view-space depth.
            // rd.position.w = 1/clip.w; clip.w = view-space Z for perspective projection.
            const float linearDepth = 1.0 / rd.position.w;
            if (light.typeCastShadows.w == 0u) {
                if (lighting.shadowBiasNormalStrength.w > 0.5) {
                    shadowFactor = evaluateDirectionalShadow(shadowTexture, lighting.shadowMatrixPalette,
                        lighting.shadowCascadeDistances, lighting.shadowCascadeParams,
                        lighting.shadowBiasNormalStrength, lighting.pcssParams, lighting.pcssCascadeRadii,
                        lighting.pcssCascadeDepthRanges, rd.worldPos, N, L, linearDepth, rd.position.xy);
                }
            } else if (lighting.shadow1BiasNormalStrength.w > 0.5) {
                shadowFactor = evaluateDirectionalShadow(shadowTexture1, lighting.shadow1MatrixPalette,
                    lighting.shadow1CascadeDistances, lighting.shadow1CascadeParams,
                    lighting.shadow1BiasNormalStrength, lighting.shadow1PcssParams, lighting.shadow1PcssCascadeRadii,
                    lighting.shadow1PcssCascadeDepthRanges, rd.worldPos, N, L, linearDepth, rd.position.xy);
            }
        }
#endif

#if VT_FEATURE_PARALLAX
        // Parallax self-shadowing: the height field casts onto itself, which the
        // cascade map cannot see because it only knows the flat polygon. Only the
        // directional light pays for the extra march.
        if (lightType == 0u && parallaxShadowActive) {
            const float3 lightDirTS = normalize(float3(
                dot(parallaxTangent, L), dot(parallaxBitangent, L),
                dot(parallaxNormalGeom, L)));
            shadowFactor *= parallaxSelfShadow(parallaxUv, lightDirTS,
                heightMapTexture, defaultSampler, material.heightMapFactor,
                saturate(material.heightMapParams.x), parallaxSurfaceDepth,
                material.heightMapParams.y);
        }
#endif

#if VT_FEATURE_SHADOW_CATCHER
        // accumulate shadow factor for shadow catcher output.
        // Shadow catcher only cares about directional light shadows.
        if (lightType == 0u) {
            dShadowCatcher *= shadowFactor;
        }
#endif

#if VT_FEATURE_AREA_LIGHTS
        if (lightShape != 0u) {
            // Area light — LTC (linearly transformed cosines). The world half axes
            // are the light's scaled X and Z.
            const float3 lightPos = light.positionRange.xyz;
            const LtcAreaLight area = ltcAreaLight(lightShape, lightPos, light.areaHalfWidth.xyz,
                light.areaHalfHeight.xyz, N, cameraPosition);
            const float3 areaRadiance = lightColor * cookieMask * lightIntensity * attenuation * shadowFactor;
            const float3 ltcSpecFres = ltcSpecularFresnel(N, V, gloss, F0, areaLightsLutTex2);

            // A directional source keeps plain Lambert; a local one
            // integrates its shape. LTC lights do not mix diffuse into the specular.
            const float ltcDiffuse = (lightType == 0u)
                ? max(dot(N, L), 0.0)
                : ltcAreaDiffuse(lightShape, area, lightPos, N, V, rd.worldPos, areaLightsLutTex2);
            directDiffuse += areaRadiance * ltcDiffuse * (float3(1.0) - ltcSpecFres);
            directSpecular += areaRadiance * ltcSpecFres *
                ltcAreaSpecular(lightShape, area, N, V, rd.worldPos, gloss, areaLightsLutTex1, areaLightsLutTex2);

#if VT_FEATURE_CLEARCOAT
            // Clearcoat LTC specular with the clearcoat normal / gloss and F0 = 0.04.
            ccSpecularLight += areaRadiance * ltcSpecularFresnel(ccNormalW, V, ccGlossiness, float3(0.04), areaLightsLutTex2) *
                ltcAreaSpecular(lightShape, area, ccNormalW, V, rd.worldPos, ccGlossiness, areaLightsLutTex1, areaLightsLutTex2);
#endif
            continue;
        }
#endif

        const float3 H = normalize(L + V);
        const float nDotL = max(dot(N, L), 0.0);
        if (nDotL <= 0.0) {
            continue;
        }
        const float3 radiance = lightColor * cookieMask * lightIntensity * attenuation * shadowFactor;
        const float nDotV = max(dot(N, V), 0.0);
        const float NoH = max(dot(N, H), 0.0);
#if VT_FEATURE_ANISOTROPY
        // Anisotropic GGX (common-brdf): D carries the whole D * Vis product.
        const float D = getLightSpecularAnisoGGX(N, V, H, L, anisoT, anisoB, anisoAlpha);
        const float G = 1.0;
#else
        const float denom = NoH * NoH * (alpha2 - 1.0) + 1.0;
        const float D = alpha2 / (PI * denom * denom);
        const float lambdaV = nDotL * sqrt(nDotV * nDotV * (1.0 - alpha4) + alpha4);
        const float lambdaL = nDotV * sqrt(nDotL * nDotL * (1.0 - alpha4) + alpha4);
        const float G = 0.5 / max(lambdaV + lambdaL, 1e-5);
#endif
        // directional lights use gloss-dependent Fresnel,
        // point/spot lights use plain specularity.
        float3 F = (lightType == 0u)
            ? getFresnel(dot(H, V), gloss, F0)
            : F0;
#if VT_FEATURE_IRIDESCENCE
        // Thin-film iridescence: blend base Fresnel toward iridescence Fresnel.
        F = mix(F, iridFresnel, iridIntensity);
#endif
        // With area lights in the variant, a punctual light's diffuse is scaled by
        // (1 - specularity) (the LTC lights take
        // (1 - their Fresnel) the same way).
#if VT_FEATURE_AREA_LIGHTS && !VT_FEATURE_NO_SPECULAR
        const float3 punctualDiffuseScale = float3(1.0) - F0;
#else
        const float3 punctualDiffuseScale = float3(1.0);
#endif
#if VT_FEATURE_OREN_NAYAR
        // Oren-Nayar rough diffuse (fast qualitative form): retro-reflection for
        // rough surfaces instead of plain Lambert.
        {
            const float sigma2 = roughness * roughness;
            const float onA = 1.0 - 0.5 * sigma2 / (sigma2 + 0.33);
            const float onB = 0.45 * sigma2 / (sigma2 + 0.09);
            const float sTerm = dot(L, V) - nDotL * nDotV;
            const float tTerm = sTerm <= 0.0 ? 1.0 : max(max(nDotL, nDotV), 1e-4);
            directDiffuse += radiance * punctualDiffuseScale * nDotL * (onA + onB * sTerm / tTerm);
        }
#else
        directDiffuse += radiance * punctualDiffuseScale * nDotL;
#endif
        directSpecular += radiance * D * G * F * nDotL;

#if VT_FEATURE_CLEARCOAT
        // Clearcoat per-light GGX.
        // Uses clearcoat normal for NdotL/NdotH, Kelemen visibility (simpler than Smith-GGX
        // since clearcoat is typically smooth), and fixed F0=0.04 Fresnel.
        {
            const float ccNdotL = max(dot(ccNormalW, L), 0.0);
            if (ccNdotL > 0.0) {
                const float3 ccH = normalize(L + V);
                const float ccNdotH = max(dot(ccNormalW, ccH), 0.0);
                const float ccLdotH = max(dot(L, ccH), 0.0);

                // GGX NDF with clearcoat alpha
                const float ccDenom = ccNdotH * ccNdotH * (ccAlpha2 - 1.0) + 1.0;
                const float ccD = ccAlpha2 / (PI * ccDenom * ccDenom);

                // Kelemen visibility (V = 0.25 / LdotH^2)
                const float ccVis = getVisibilityKelemen(ccLdotH);

                // Schlick Fresnel with fixed F0=0.04
                const float ccF = getFresnelCC(ccLdotH);

                ccSpecularLight += radiance * ccNdotL * ccD * ccVis * ccF;
            }
        }
#endif

#if VT_FEATURE_SHEEN
        // Sheen per-light: Charlie distribution + Ashikhmin visibility.
        // Uses the same N, L, V, H, nDotL, nDotV, radiance as the main BRDF.
        {
            const float sheenD = sheenDistribution(NoH, sheenRoughness);
            const float sheenV = sheenVisibility(nDotV, nDotL);
            sheenSpecularDirect += radiance * nDotL * sheenD * sheenV * sheenTint;
        }
#endif
    }
