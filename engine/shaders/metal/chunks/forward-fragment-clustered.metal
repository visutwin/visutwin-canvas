// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
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

                // Clustered shadow: sample this light's rect of the shadow atlas.
                // shadowData: x=castShadows, y=normalOffsetBias, z=intensity,
                // w=1 spot / 2 omni. A spot projects through its viewport-aware VP;
                // an omni picks a cube face from the direction (shadowMatrix then
                // carries its rect and depth range instead of a matrix).
                // A spot's compare depth carries NO bias of its own: the atlas pass
                // already applied hardware polygon offset, and a shader bias on this
                // projection is hopeless — a spot with near 0.01 and range 150 crushes
                // the whole scene into ~0.001 of depth, which any bias worth the name
                // swamps. Only the receiver's normal offset is applied, as upstream does.
                if (cl.shadowData.x > 0.5) {
                    const float res = float(clusterShadowAtlas.get_width());
                    if (cl.shadowData.w > 1.5) {
                        // Omni receiver offset is upstream's normalOffsetPointShadow:
                        // the GEOMETRIC normal, scaled by normalBias, by how grazing
                        // the light is (1 - NdotL) and by the DISTANCE to the light.
                        // A torch mounted on its own wall lights that wall at ~90
                        // degrees from tens of units away; the flat `N * normalBias`
                        // the spot below uses (0.2 units here) is then ~30x too small
                        // and every face texel self-shadows in radial streaks.
                        float3 Ng = normalize(rd.worldNormal);
                        if (dot(Ng, N) < 0.0) Ng = -Ng;
                        const float3 toLight = normalize(lightDirW);
                        const float grazing = clamp(1.0 - dot(Ng, toLight), 0.0, 1.0);
                        const float3 shadowPosW = rd.worldPos
                            + Ng * (cl.shadowData.y * grazing * length(lightDirW));
                        if (res > 0.0) {
                            const float vis = getShadowOmniClusteredPCF3(clusterShadowAtlas,
                                cl.shadowMatrix[0], cl.shadowMatrix[1],
                                shadowPosW - cl.positionRange.xyz);
                            attenuation *= mix(1.0, vis, cl.shadowData.z);
                        }
                    } else {
                        // A spot's offset is the flat normalBias, as upstream's
                        // getShadowCoordPerspZbufferNormalOffset.
                        const float3 shadowPosW = rd.worldPos + N * cl.shadowData.y;
                        const float4 sc = cl.shadowMatrix * float4(shadowPosW, 1.0);
                        const float sw = max(sc.w, 1e-6);
                        const float3 scoord = sc.xyz / sw;
                        if (scoord.x >= 0.0 && scoord.x <= 1.0 &&
                            scoord.y >= 0.0 && scoord.y <= 1.0 &&
                            scoord.z >= 0.0 && scoord.z <= 1.0 && res > 0.0) {
                            const float vis = getShadowPCF3x3(clusterShadowAtlas, scoord.xy,
                                scoord.z, res);
                            attenuation *= mix(1.0, vis, cl.shadowData.z);
                        }
                    }
                }

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
                // Anisotropic GGX (common-brdf): clD carries the whole D * Vis product.
                const float clD = getLightSpecularAnisoGGX(N, V, clH, clL, anisoT, anisoB, anisoAlpha);
                const float clG = 1.0;
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

                #if VT_FEATURE_OREN_NAYAR
                {
                    const float sigma2 = roughness * roughness;
                    const float onA = 1.0 - 0.5 * sigma2 / (sigma2 + 0.33);
                    const float onB = 0.45 * sigma2 / (sigma2 + 0.09);
                    const float sTerm = dot(clL, V) - clNdotL * clNdotV;
                    const float tTerm = sTerm <= 0.0 ? 1.0 : max(max(clNdotL, clNdotV), 1e-4);
                    directDiffuse += clRadiance * clNdotL * (onA + onB * sTerm / tTerm);
                }
#else
                directDiffuse += clRadiance * clNdotL;
#endif
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
