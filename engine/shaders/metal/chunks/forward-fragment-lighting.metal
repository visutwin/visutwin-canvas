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
            // Area rectangular light — Most Representative Point (MRP) technique.
            // For diffuse: closest point on rectangle to the fragment.
            // For specular: closest point on rectangle to the reflected ray.
            // Existing GGX NDF runs unchanged with the MRP-computed L.
            const float3 lightPos = light.positionRange.xyz;
            const float3 lightNrm = normalize(light.directionCone.xyz);
            const float halfW = light.directionCone.w;
            const float halfH = light.coneAngles.x;
            const float3 right = normalize(float3(light.coneAngles.y, light.coneAngles.z, light.coneAngles.w));
            const float3 up = normalize(cross(lightNrm, right));
            const float3 toCenter = lightPos - rd.worldPos;

            // Hemisphere check: only illuminate from the emitting face.
            // Light direction convention: entity -Y axis = emission direction,
            // so the fragment must be on the emitting side (dot < 0 from light's perspective).
            if (dot(toCenter, lightNrm) > 0.0) {
                attenuation = 0.0;
            } else {
                // Diffuse: closest point on rectangle to fragment.
                const float pR = clamp(-dot(toCenter, right), -halfW, halfW);
                const float pU = clamp(-dot(toCenter, up), -halfH, halfH);
                const float3 closestPt = lightPos + right * pR + up * pU;
                lightDirW = closestPt - rd.worldPos;

                // Specular: closest point on rectangle to reflected ray.
                const float3 R = reflect(-V, N);
                const float dRP = dot(R, lightNrm);
                if (abs(dRP) > 1e-6) {
                    const float t = dot(toCenter, lightNrm) / dRP;
                    if (t > 0.0) {
                        const float3 hit = rd.worldPos + R * t;
                        const float3 off = hit - lightPos;
                        const float sR = clamp(dot(off, right), -halfW, halfW);
                        const float sU = clamp(dot(off, up), -halfH, halfH);
                        L = normalize(lightPos + right * sR + up * sU - rd.worldPos);
                    } else {
                        L = normalize(lightDirW);
                    }
                } else {
                    L = normalize(lightDirW);
                }

                // Attenuation from closest-point distance.
                if (falloffModeLinear) {
                    attenuation = getFalloffLinear(light.positionRange.w, lightDirW);
                } else {
                    attenuation = getFalloffInvSquared(light.positionRange.w, lightDirW);
                }
            }
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
        directDiffuse += radiance * nDotL;
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

#if VT_FEATURE_LIGHT_CLUSTERING
    // === Clustered local lights: sample from 3D grid ===
    // WorldClusters GPU sampling.
    // Directional lights remain in LightingData.lights[] above.
    // Local lights (point/spot) are indexed via a 3D cell grid.
    {
        // 1. Convert world position to cell coordinates.
        const float3 cellCoord = (rd.worldPos - lighting.clusterBoundsMin.xyz)
                                 * lighting.clusterCellsCountByBoundsSize.xyz;
        const int3 cell = int3(floor(cellCoord));

        // 2. Bounds check: skip if fragment is outside the cluster grid.
        const int3 cellsMax = int3(lighting.clusterParams.xyz);
        if (all(cell >= int3(0)) && all(cell < cellsMax)) {
            // 3. Linear cell index into flat cell data array.
            const uint maxPerCell = lighting.clusterParams.w;
            const uint cellIndex = (uint(cell.y) * uint(cellsMax.x) * uint(cellsMax.z)
                                  + uint(cell.z) * uint(cellsMax.x)
                                  + uint(cell.x)) * maxPerCell;

            // 4. Loop over lights in this cell.
            for (uint s = 0u; s < maxPerCell; ++s) {
                const uint lightIdx1 = uint(clusterCells[cellIndex + s]);
                if (lightIdx1 == 0u) break;  // 0 = no more lights in this cell
                const uint lightIdx = lightIdx1 - 1u;  // convert 1-based to 0-based

                const ClusteredLight cl = clusterLights[lightIdx];

                // 5. Compute attenuation (reuse existing falloff functions from common.metal).
                const float3 lightDirW = cl.positionRange.xyz - rd.worldPos;
                float attenuation;
                if (cl.params.z > 0.5) {
                    attenuation = getFalloffLinear(cl.positionRange.w, lightDirW);
                } else {
                    attenuation = getFalloffInvSquared(cl.positionRange.w, lightDirW);
                }

                // Spot cone attenuation.
                if (cl.params.y > 0.5) {
                    const float3 spotDir = normalize(cl.directionSpot.xyz);
                    const float outerConeCos = cl.directionSpot.w;
                    const float innerConeCos = cl.params.x;
                    const float3 dLightDirNormW = normalize(lightDirW);
                    attenuation *= getSpotEffect(spotDir, innerConeCos, outerConeCos, -dLightDirNormW);
                }

                if (attenuation < 0.00001) continue;

                // 6. PBR lighting (same GGX terms as the main light loop).
                const float3 clL = normalize(lightDirW);
                const float clNdotL = max(dot(N, clL), 0.0);
                if (clNdotL <= 0.0) continue;

                const float3 clH = normalize(clL + V);
                const float3 clRadiance = max(cl.colorIntensity.xyz, float3(0.0))
                                        * max(cl.colorIntensity.w, 0.0) * attenuation;
                const float clNdotV = max(dot(N, V), 0.0);
                const float clNoH = max(dot(N, clH), 0.0);
#if VT_FEATURE_ANISOTROPY
                // Anisotropic GGX for clustered lights.
                const float clTdotH = dot(anisoT, clH);
                const float clBdotH = dot(anisoB, clH);
                const float clTdotL = dot(anisoT, clL);
                const float clBdotL = dot(anisoB, clL);
                const float clTdotV = dot(anisoT, V);
                const float clBdotV = dot(anisoB, V);
                const float clAnisoF = clTdotH * clTdotH / anisoAt2
                                      + clBdotH * clBdotH / anisoAb2 + clNoH * clNoH;
                const float clD = 1.0 / (PI * anisoAt * anisoAb
                                          * max(clAnisoF * clAnisoF, 1e-8));
                const float clLambdaV = clNdotL * sqrt(anisoAt2 * clTdotV * clTdotV
                                        + anisoAb2 * clBdotV * clBdotV + clNdotV * clNdotV);
                const float clLambdaL = clNdotV * sqrt(anisoAt2 * clTdotL * clTdotL
                                        + anisoAb2 * clBdotL * clBdotL + clNdotL * clNdotL);
                const float clG = 0.5 / max(clLambdaV + clLambdaL, 1e-5);
#else
                const float clDenom = clNoH * clNoH * (alpha2 - 1.0) + 1.0;
                const float clD = alpha2 / (PI * clDenom * clDenom);
                const float clLambdaV = clNdotL * sqrt(clNdotV * clNdotV * (1.0 - alpha4) + alpha4);
                const float clLambdaL = clNdotV * sqrt(clNdotL * clNdotL * (1.0 - alpha4) + alpha4);
                const float clG = 0.5 / max(clLambdaV + clLambdaL, 1e-5);
#endif
                // point/spot lights use plain specularity (no gloss-dependent Fresnel).
                float3 clF = F0;
#if VT_FEATURE_IRIDESCENCE
                clF = mix(clF, iridFresnel, iridIntensity);
#endif

                directDiffuse += clRadiance * clNdotL;
                directSpecular += clRadiance * clD * clG * clF * clNdotL;

#if VT_FEATURE_CLEARCOAT
                // Clearcoat per-light GGX for clustered lights.
                {
                    const float ccClNdotL = max(dot(ccNormalW, clL), 0.0);
                    if (ccClNdotL > 0.0) {
                        const float3 ccClH = normalize(clL + V);
                        const float ccClNdotH = max(dot(ccNormalW, ccClH), 0.0);
                        const float ccClLdotH = max(dot(clL, ccClH), 0.0);
                        const float ccClDenom2 = ccClNdotH * ccClNdotH * (ccAlpha2 - 1.0) + 1.0;
                        const float ccClD = ccAlpha2 / (PI * ccClDenom2 * ccClDenom2);
                        const float ccClVis = getVisibilityKelemen(ccClLdotH);
                        const float ccClF = getFresnelCC(ccClLdotH);
                        ccSpecularLight += clRadiance * ccClNdotL * ccClD * ccClVis * ccClF;
                    }
                }
#endif

#if VT_FEATURE_SHEEN
                // Sheen per-clustered-light.
                {
                    const float clSheenNoH = max(dot(N, clH), 0.0);
                    const float clSheenD = sheenDistribution(clSheenNoH, sheenRoughness);
                    const float clSheenV = sheenVisibility(clNdotV, clNdotL);
                    sheenSpecularDirect += clRadiance * clNdotL * clSheenD * clSheenV * sheenTint;
                }
#endif
            }
        }
    }
#endif

    // ambient diffuse (no energy conservation).
    float3 indirectDiffuse = max(lighting.ambientColor.xyz, float3(0.0));
    float3 indirectSpecular = float3(0.0);
#if VT_FEATURE_ENV_ATLAS
    if (envAtlasTexture.get_width() > 0 && envAtlasTexture.get_height() > 0) {
        // Diffuse IBL: sample from dedicated Lambert irradiance sub-region.
        // Single-path sampling matching upstream ambient.js — the atlas
        // is pre-baked with 1-pixel duplicated seam borders, so the default
        // sampler produces continuous values across the ±180° wrap.
        const float3 diffDir = float3(-N.x, N.y, N.z);
        const float2 envUvN = toSphericalUv(normalize(diffDir));
        const float3 envAmbient = processEnvironment(
            decodeEnvironment(envAtlasTexture.sample(defaultSampler, mapAmbientUv(envUvN)), lighting),
            max(lighting.cameraPositionSkyboxIntensity.w, 0.0));
        indirectDiffuse = envAmbient;

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

        // Single-path specular IBL matching upstream reflectionEnv.js.
        // Atlas is baked with duplicated seam pixels (see envLighting.cpp),
        // so the default anisotropic sampler handles the ±180° wrap
        // correctly without runtime branching. The upstream
        // `shinyMipLevel` uses a second-derivative trick (dFdx/dFdy on
        // fract(u+0.5)) to avoid the gradient spike at the wrap — already
        // replicated in the shinyUvFull/uv2/dx2/dy2 computation above.
        float3 linear0, linear1;
        if (ilevel == 0.0) {
            linear0 = decodeEnvironment(envAtlasTexture.sample(defaultSampler, mapShinyUv(envUvSpec, ilevel2)), lighting);
            linear1 = decodeEnvironment(envAtlasTexture.sample(defaultSampler, mapShinyUv(envUvSpec, ilevel2 + 1.0)), lighting);
            linear0 = mix(linear0, linear1, level2 - ilevel2);
        } else {
            linear0 = decodeEnvironment(envAtlasTexture.sample(defaultSampler, mapRoughnessUv(envUvSpec, ilevel)), lighting);
        }
        linear1 = decodeEnvironment(envAtlasTexture.sample(defaultSampler, mapRoughnessUv(envUvSpec, ilevel + 1.0)), lighting);

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
                const float3 a = decodeEnvironment(envAtlasTexture.sample(defaultSampler, mapShinyUv(ccEnvUv, 0.0)), lighting);
                const float3 b = decodeEnvironment(envAtlasTexture.sample(defaultSampler, mapShinyUv(ccEnvUv, 1.0)), lighting);
                ccEnvColor = mix(a, b, ccLevel);
            } else {
                const float3 a = decodeEnvironment(envAtlasTexture.sample(defaultSampler, mapRoughnessUv(ccEnvUv, ccIlevel)), lighting);
                const float3 b = decodeEnvironment(envAtlasTexture.sample(defaultSampler, mapRoughnessUv(ccEnvUv, ccIlevel + 1.0)), lighting);
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
                const float3 a = decodeEnvironment(envAtlasTexture.sample(defaultSampler, mapShinyUv(sheenEnvUv, 0.0)), lighting);
                const float3 b = decodeEnvironment(envAtlasTexture.sample(defaultSampler, mapShinyUv(sheenEnvUv, 1.0)), lighting);
                sheenEnvColor = mix(a, b, sheenLevel);
            } else {
                const float3 a = decodeEnvironment(envAtlasTexture.sample(defaultSampler, mapRoughnessUv(sheenEnvUv, sheenILevel)), lighting);
                const float3 b = decodeEnvironment(envAtlasTexture.sample(defaultSampler, mapRoughnessUv(sheenEnvUv, sheenILevel + 1.0)), lighting);
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
    float3 emissiveLinear = max(material.emissiveColor.rgb, float3(0.0));
#if VT_FEATURE_EMISSIVE_MAP
    if (emissiveTexture.get_width() > 0 && emissiveTexture.get_height() > 0) {
        emissiveLinear *= srgbToLinear(emissiveTexture.sample(defaultSampler, uvEmissive).rgb);
    }
#endif
