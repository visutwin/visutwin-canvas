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

        float3 L = float3(0.0, 1.0, 0.0);
        float attenuation = 1.0;
        float3 lightDirW = float3(0.0);
        if (lightType == 0u) {
            const float3 lightDir = light.directionCone.xyz;
            if (length_squared(lightDir) <= 1e-8) {
                continue;
            }
            L = -normalize(lightDir);
        }
#if VT_FEATURE_AREA_LIGHTS
        else if (lightType == 3u) {
            // Area rectangular light — LTC (linearly transformed cosines), port of
            // upstream ltc.js rect path. Diffuse and specular are both polygonal
            // integrals; the shared punctual GGX below is skipped entirely.
            const float3 lightPos = light.positionRange.xyz;
            const float halfW = light.directionCone.w;
            const float halfH = light.coneAngles.x;
            const float3 lightNrm = normalize(light.directionCone.xyz);
            const float3 right = normalize(float3(light.coneAngles.y, light.coneAngles.z, light.coneAngles.w));
            const float3 up = normalize(cross(lightNrm, right));
            const float3 halfWidthVec = right * halfW;
            const float3 halfHeightVec = up * halfH;

            // Rect corners, ccw (upstream getLTCLightCoords).
            const float3 p0 = lightPos + halfWidthVec - halfHeightVec;
            const float3 p1 = lightPos - halfWidthVec - halfHeightVec;
            const float3 p2 = lightPos - halfWidthVec + halfHeightVec;
            const float3 p3 = lightPos + halfWidthVec + halfHeightVec;

            // Non-punctual lights only get the range window here — the physical
            // distance falloff comes from the LTC form factor itself.
            lightDirW = lightPos - rd.worldPos;
            const float areaAtten = getFalloffWindow(light.positionRange.w, lightDirW);
            if (areaAtten < 0.00001) {
                continue;
            }
            const float3 areaRadiance = lightColor * lightIntensity * areaAtten;

            // LUT2: Fresnel magnitude (x) + geometric attenuation (y) for
            // specular energy conservation.
            const float2 ltcLutUv = ltcUv(N, V, gloss);
            const float4 ltcT2 = areaLightsLutTex2.sample(ltcLutSampler, ltcLutUv, level(0));
            const float3 ltcSpecFres = F0 * ltcT2.x + (float3(1.0) - F0) * ltcT2.y;

            // Diffuse: LTC with identity transform (plain cosine integral).
            // 16.0 mirrors the constant baked into getFalloffInvSquared so area and
            // punctual lights of equal intensity have comparable brightness.
            const float ltcDiffuse = ltcEvaluateRect(N, V, rd.worldPos,
                float3x3(float3(1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 1.0)),
                p0, p1, p2, p3) * 16.0;
            directDiffuse += areaRadiance * ltcDiffuse * (float3(1.0) - ltcSpecFres);

            // Specular: LTC with the inverse transform fetched from LUT1.
            const float4 ltcT1 = areaLightsLutTex1.sample(ltcLutSampler, ltcLutUv, level(0));
            const float3x3 ltcMInv = float3x3(
                float3(ltcT1.x, 0.0, ltcT1.y),
                float3(0.0, 1.0, 0.0),
                float3(ltcT1.z, 0.0, ltcT1.w));
            const float ltcSpec = ltcEvaluateRect(N, V, rd.worldPos, ltcMInv, p0, p1, p2, p3);
            directSpecular += areaRadiance * ltcSpec * ltcSpecFres;

#if VT_FEATURE_CLEARCOAT
            // Clearcoat LTC specular with the clearcoat normal/gloss and fixed F0=0.04.
            {
                const float2 ccLtcUv = ltcUv(ccNormalW, V, ccGlossiness);
                const float4 ccT2 = areaLightsLutTex2.sample(ltcLutSampler, ccLtcUv, level(0));
                const float3 ccSpecFres = float3(0.04) * ccT2.x + float3(0.96) * ccT2.y;
                const float4 ccT1 = areaLightsLutTex1.sample(ltcLutSampler, ccLtcUv, level(0));
                const float3x3 ccMInv = float3x3(
                    float3(ccT1.x, 0.0, ccT1.y),
                    float3(0.0, 1.0, 0.0),
                    float3(ccT1.z, 0.0, ccT1.w));
                ccSpecularLight += areaRadiance * ltcEvaluateRect(ccNormalW, V, rd.worldPos, ccMInv, p0, p1, p2, p3) * ccSpecFres;
            }
#endif

            // Area light fully accumulated — skip the shared punctual shadow/GGX path.
            // DEVIATION: area lights do not cast/receive local shadows in this port.
            continue;
        }
#endif
        else {
            lightDirW = light.positionRange.xyz - rd.worldPos;
            const float lightDirLenSq = dot(lightDirW, lightDirW);
            if (lightDirLenSq <= 1e-8) {
                continue;
            }
            const float invLightDirLen = rsqrt(lightDirLenSq);
            const float3 dLightDirNormW = lightDirW * invLightDirLen;
            L = dLightDirNormW;
#if VT_FEATURE_POINT_SPOT_ATTENUATION
            if (falloffModeLinear) {
                attenuation = getFalloffLinear(light.positionRange.w, lightDirW);
            } else {
                attenuation = getFalloffInvSquared(light.positionRange.w, lightDirW);
            }

            if (lightType == 2u) {
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

            // Apply normal bias in world space, scaled by sin(angle) between
            // normal and light direction so grazing surfaces get more offset.
            const float localNdotL = saturate(dot(N, L));
            const float localSinAngle = sqrt(1.0 - localNdotL * localNdotL);
            const float3 biasedPos = rd.worldPos + N * (shadowParamsLocal.y * localSinAngle);
            const float4 shadowClip = shadowMatrix * float4(biasedPos, 1.0);
            const float shadowW = max(shadowClip.w, 1e-6);
            const float3 shadowCoord = shadowClip.xyz / shadowW;

            if (shadowCoord.x >= 0.0 && shadowCoord.x <= 1.0 &&
                shadowCoord.y >= 0.0 && shadowCoord.y <= 1.0 &&
                shadowCoord.z >= 0.0 && shadowCoord.z <= 1.0) {

                const float receiverDepth = shadowCoord.z - shadowParamsLocal.x;
                float visible = 1.0;
                if (shadowIdx == 0u) {
                    const float res0 = float(localShadowTexture0.get_width());
                    if (res0 > 0.0) {
                        visible = getShadowPCF3x3(localShadowTexture0, shadowCoord.xy, receiverDepth, res0);
                    }
                } else {
                    const float res1 = float(localShadowTexture1.get_width());
                    if (res1 > 0.0) {
                        visible = getShadowPCF3x3(localShadowTexture1, shadowCoord.xy, receiverDepth, res1);
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
            const float denom = (far_val - near_val) * d;
            const float compareValue = far_val * (d - near_val) / max(denom, 1e-6) - bias;

            constexpr sampler omniShadowSampler(coord::normalized, filter::linear,
                                                compare_func::less_equal, address::clamp_to_edge);

            float visible = 1.0;
            if (shadowIdx == 0u) {
                visible = omniShadowCube0.sample_compare(omniShadowSampler, lightToFrag, compareValue);
            } else {
                visible = omniShadowCube1.sample_compare(omniShadowSampler, lightToFrag, compareValue);
            }

            shadowFactor = mix(1.0 - clamp(intensity, 0.0, 1.0), 1.0, visible);
        }
#endif

#if VT_FEATURE_SHADOWS
        if (!shadowApplied && lightType == 0u && lightCastsShadows && lighting.shadowBiasNormalStrength.w > 0.5 &&
            shadowTexture.get_width() > 0 && shadowTexture.get_height() > 0) {
            // CSM: determine cascade from fragment's linear view-space depth.
            // rd.position.w = 1/clip.w; clip.w = view-space Z for perspective projection.
            const float linearDepth = 1.0 / rd.position.w;
            const int cascadeCount = int(lighting.shadowCascadeParams.x);
            const int cascadeIndex = getShadowCascadeIndex(
                lighting.shadowCascadeDistances, cascadeCount, linearDepth);

            // Apply normal bias in world space, scaled by sin(angle) between
            // normal and light direction so grazing surfaces get more offset
            // while directly-lit faces get almost none — prevents light leaking
            // at triangle edges on curved geometry.
            const float csmNdotL = saturate(dot(N, L));
            const float csmSinAngle = sqrt(1.0 - csmNdotL * csmNdotL);
            const float3 worldPosBiased = rd.worldPos + N * (lighting.shadowBiasNormalStrength.y * csmSinAngle);

            // Transform world position via the cascade's viewport-scaled shadow matrix.
            // The matrix already bakes in projection, view, NDC-to-atlas-UV, Metal Y-flip,
            // and Z [0,1] mapping — no manual coordinate conversion needed.
            const float4 shadowClip = lighting.shadowMatrixPalette[cascadeIndex] * float4(worldPosBiased, 1.0);
            const float shadowW = max(shadowClip.w, 1e-6);
            const float3 shadowCoord = shadowClip.xyz / shadowW;

            const float2 shadowUv = shadowCoord.xy;
            const float shadowDepth = shadowCoord.z;

            const float resolution = float(shadowTexture.get_width());
            const bool insideShadow = shadowUv.x >= 0.0 && shadowUv.x <= 1.0 &&
                shadowUv.y >= 0.0 && shadowUv.y <= 1.0 &&
                shadowDepth >= 0.0 && shadowDepth <= 1.0;
            if (insideShadow) {
#if VT_FEATURE_VSM_SHADOWS
                // EVSM_16F — sample exponentially-warped moments and reconstruct
                // visibility via Chebyshev's inequality. No depth-bias subtraction —
                // VSM uses a separate vsmBias inside calculateEVSM.
                const float vsmBias = max(lighting.shadowBiasNormalStrength.x, 1e-4);
                const float visible = getShadowVSM16(shadowTexture, shadowUv, shadowDepth, vsmBias);
#elif VT_FEATURE_PCSS_SHADOWS
                // PCSS — contact-hardening soft shadows: Vogel-disk blocker search
                // sets a world-space penumbra per fragment.
                const float pcssDepth = shadowDepth - lighting.shadowBiasNormalStrength.x;
                const float visible = getShadowPCSSDirectional(shadowTexture,
                    float3(shadowUv, pcssDepth),
                    lighting.pcssCascadeRadii[cascadeIndex],
                    lighting.pcssCascadeDepthRanges[cascadeIndex],
                    lighting.pcssParams, rd.position.xy);
#else
                // PCF3_32F — optimized bilinear 3×3 PCF.
                const float receiverDepth = shadowDepth - lighting.shadowBiasNormalStrength.x;
                const float visible = getShadowPCF3x3(shadowTexture, shadowUv, receiverDepth, resolution);
#endif
                shadowFactor = mix(1.0 - clamp(lighting.shadowBiasNormalStrength.z, 0.0, 1.0), 1.0, visible);
            }

            // CSM: cross-cascade blending near each cascade boundary.
            // Sample the next cascade and blend to eliminate the hard transition
            // that otherwise creates visible bright lines on geometry.
            const float cascadeBlendWidth = lighting.shadowCascadeParams.y;
            if (cascadeBlendWidth > 0.0 && cascadeIndex < cascadeCount - 1) {
                const float cascadeFar = lighting.shadowCascadeDistances[cascadeIndex];
                const float fade = saturate((cascadeFar - linearDepth) / cascadeBlendWidth);
                if (fade < 1.0) {
                    // Sample shadow from the next cascade for cross-fade.
                    const int nextCascade = cascadeIndex + 1;
                    const float4 nextShadowClip = lighting.shadowMatrixPalette[nextCascade] * float4(worldPosBiased, 1.0);
                    const float nextW = max(nextShadowClip.w, 1e-6);
                    const float3 nextCoord = nextShadowClip.xyz / nextW;
                    float nextShadowFactor = 1.0;
                    if (nextCoord.x >= 0.0 && nextCoord.x <= 1.0 &&
                        nextCoord.y >= 0.0 && nextCoord.y <= 1.0 &&
                        nextCoord.z >= 0.0 && nextCoord.z <= 1.0) {
#if VT_FEATURE_VSM_SHADOWS
                        const float nextVsmBias = max(lighting.shadowBiasNormalStrength.x, 1e-4);
                        const float nextVisible = getShadowVSM16(shadowTexture, nextCoord.xy, nextCoord.z, nextVsmBias);
#elif VT_FEATURE_PCSS_SHADOWS
                        const float nextPcssDepth = nextCoord.z - lighting.shadowBiasNormalStrength.x;
                        const float nextVisible = getShadowPCSSDirectional(shadowTexture,
                            float3(nextCoord.xy, nextPcssDepth),
                            lighting.pcssCascadeRadii[nextCascade],
                            lighting.pcssCascadeDepthRanges[nextCascade],
                            lighting.pcssParams, rd.position.xy);
#else
                        const float nextReceiverDepth = nextCoord.z - lighting.shadowBiasNormalStrength.x;
                        const float nextVisible = getShadowPCF3x3(shadowTexture, nextCoord.xy, nextReceiverDepth, resolution);
#endif
                        nextShadowFactor = mix(1.0 - clamp(lighting.shadowBiasNormalStrength.z, 0.0, 1.0), 1.0, nextVisible);
                    }
                    shadowFactor = mix(nextShadowFactor, shadowFactor, fade);
                }
            }

            // Fade shadow at max distance to avoid hard cutoff.
            const float maxDist = lighting.shadowCascadeDistances[cascadeCount - 1];
            const float fadeStart = maxDist * 0.9;
            if (linearDepth > fadeStart && maxDist > fadeStart) {
                shadowFactor = mix(shadowFactor, 1.0, saturate((linearDepth - fadeStart) / (maxDist - fadeStart)));
            }

            shadowApplied = true;
        }
#endif

#if VT_FEATURE_SHADOW_CATCHER
        // accumulate shadow factor for shadow catcher output.
        // Shadow catcher only cares about directional light shadows.
        if (lightType == 0u) {
            dShadowCatcher *= shadowFactor;
        }
#endif

        const float3 H = normalize(L + V);
        const float nDotL = max(dot(N, L), 0.0);
        if (nDotL <= 0.0) {
            continue;
        }
        const float3 radiance = lightColor * lightIntensity * attenuation * shadowFactor;
        const float nDotV = max(dot(N, V), 0.0);
        const float NoH = max(dot(N, H), 0.0);
#if VT_FEATURE_ANISOTROPY
        // Anisotropic GGX NDF (Burley 2012) + Smith-GGX visibility.
        // Anisotropic specular GGX.
        const float TdotH = dot(anisoT, H);
        const float BdotH = dot(anisoB, H);
        const float TdotL = dot(anisoT, L);
        const float BdotL = dot(anisoB, L);
        const float TdotV = dot(anisoT, V);
        const float BdotV = dot(anisoB, V);
        const float anisoF = TdotH * TdotH / anisoAt2
                            + BdotH * BdotH / anisoAb2 + NoH * NoH;
        const float D = 1.0 / (PI * anisoAt * anisoAb
                                * max(anisoF * anisoF, 1e-8));
        const float lambdaV = nDotL * sqrt(anisoAt2 * TdotV * TdotV
                              + anisoAb2 * BdotV * BdotV + nDotV * nDotV);
        const float lambdaL = nDotV * sqrt(anisoAt2 * TdotL * TdotL
                              + anisoAb2 * BdotL * BdotL + nDotL * nDotL);
        const float G = 0.5 / max(lambdaV + lambdaL, 1e-5);
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
#if VT_FEATURE_OREN_NAYAR
        // Oren-Nayar rough diffuse (fast qualitative form): retro-reflection for
        // rough surfaces instead of plain Lambert.
        {
            const float sigma2 = roughness * roughness;
            const float onA = 1.0 - 0.5 * sigma2 / (sigma2 + 0.33);
            const float onB = 0.45 * sigma2 / (sigma2 + 0.09);
            const float sTerm = dot(L, V) - nDotL * nDotV;
            const float tTerm = sTerm <= 0.0 ? 1.0 : max(max(nDotL, nDotV), 1e-4);
            directDiffuse += radiance * nDotL * (onA + onB * sTerm / tTerm);
        }
#else
        directDiffuse += radiance * nDotL;
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
