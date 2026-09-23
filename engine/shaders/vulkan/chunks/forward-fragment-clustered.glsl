    if (vtFeatureEnabled(VT_FEATURE_LIGHT_CLUSTERING_BIT)) {
        ivec3 cell = ivec3(floor((fragWorldPos -
            lighting.clusterBoundsMin.xyz) *
            lighting.clusterCellsCountByBoundsSize.xyz));
        ivec3 dims = ivec3(lighting.clusterParams.xyz);
        if (all(greaterThanEqual(cell, ivec3(0))) &&
            all(lessThan(cell, dims))) {
            uint maxPerCell = lighting.clusterParams.w;
            uint base = (uint(cell.y) * uint(dims.x) * uint(dims.z) +
                uint(cell.z) * uint(dims.x) + uint(cell.x)) * maxPerCell;
            for (uint slot = 0u; slot < maxPerCell; ++slot) {
                uint index1 = clusterCells.values[base + slot];
                if (index1 == 0u) break;
                ClusterLight cl = clusterLights.values[index1 - 1u];
                vec3 delta = cl.positionRange.xyz - fragWorldPos;
                float distance = length(delta);
                vec3 L = delta / max(distance, 1e-5);
                float atten = distanceAttenuation(distance,
                    cl.positionRange.w, cl.params.z);
                if (cl.params.y > 0.5) {
                    float cone = dot(normalize(-cl.directionSpot.xyz), L);
                    atten *= getSpotEffect(cl.params.x, cl.directionSpot.w, cone);
                }
                if (atten < 1e-5) continue;

                // Clustered shadow: each shadow-casting light owns a rect of the
                // atlas. shadowData = {castShadows, normalOffsetBias, intensity,
                // 1 spot / 2 omni}. Mirrors forward-fragment-clustered.metal: a
                // receiver normal offset and an intensity blend; a spot projects
                // through its viewport-aware VP with NO depth bias (the atlas pass
                // biases on render, and this projection's depth is far too crushed
                // for a shader bias to be harmless), an omni picks a cube face from
                // the direction and takes the cubemap path's relative bias.
                if (cl.shadowData.x > 0.5) {
                    if (cl.shadowData.w > 1.5) {
                        // Omni receiver offset is upstream's normalOffsetPointShadow
                        // (see the Metal twin): geometric normal * normalBias *
                        // (1 - NdotL) * distance to the light.
                        vec3 Ng = normalize(fragWorldNormal);
                        if (dot(Ng, N) < 0.0) Ng = -Ng;
                        float grazing = clamp(1.0 - dot(Ng, L), 0.0, 1.0);
                        vec3 shadowPosW = fragWorldPos + Ng * (cl.shadowData.y * grazing * distance);
                        float vis = getShadowOmniClusteredPCF3(cl.shadowMatrix[0], cl.shadowMatrix[1],
                            shadowPosW - cl.positionRange.xyz);
                        atten *= mix(1.0, vis, clamp(cl.shadowData.z, 0.0, 1.0));
                    } else {
                        // A spot keeps the flat normalBias, as upstream.
                        vec3 shadowPosW = fragWorldPos + N * cl.shadowData.y;
                        vec4 sc = cl.shadowMatrix * vec4(shadowPosW, 1.0);
                        if (sc.w > 0.0) {
                            vec3 scoord = sc.xyz / sc.w;
                            if (all(greaterThanEqual(scoord, vec3(0.0))) &&
                                all(lessThanEqual(scoord, vec3(1.0)))) {
                                float vis = pcf3x3Atlas(scoord.xy, scoord.z);
                                atten *= mix(1.0, vis, clamp(cl.shadowData.z, 0.0, 1.0));
                            }
                        }
                    }
                }

                float nl = max(dot(N, L), 0.0);
                vec3 H = normalize(L + V);
                float nh = max(dot(N, H), 0.0);
                float vh = max(dot(V, H), 0.0);
                // Same BRDF as the non-clustered loop (common-brdf.glsl). Clustered
                // lights are punctual, so the Fresnel is bare specularity — the
                // gloss-aware curve is the directional case only.
                float D = distributionGGX(nh, roughness);
                float Vis = getVisibilitySmithGGX(NdotV, nl, roughness);
                if (vtFeatureEnabled(VT_FEATURE_ANISOTROPY_BIT)) {
                    // Anisotropic GGX (common-brdf): D carries the whole D * Vis product.
                    D = getLightSpecularAnisoGGX(N, V, H, L, anisoT, anisoB, anisoAlpha);
                    Vis = 1.0;
                }
                vec3 F = F0;
                if (vtFeatureEnabled(VT_FEATURE_IRIDESCENCE_BIT)) {
                    F = mix(F, iridFresnel, iridIntensity);
                }
                vec3 radiance = cl.colorIntensity.rgb *
                    cl.colorIntensity.w * atten;
                // Oren-Nayar, iridescence, clearcoat and sheen below are the main
                // loop's terms (forward-fragment-lights.glsl), and the Metal twin has
                // all four. This loop had none of them until 2026-09-23 — and with
                // clustered lighting the default, it is the loop EVERY spot and omni
                // light goes through, so a coated, sheened, rough-diffuse or
                // iridescent material lost that term under every local light.
                float diffuseTerm = 1.0;
                if (vtFeatureEnabled(VT_FEATURE_OREN_NAYAR_BIT)) {
                    float sigma2 = roughness * roughness;
                    float onA = 1.0 - 0.5 * sigma2 / (sigma2 + 0.33);
                    float onB = 0.45 * sigma2 / (sigma2 + 0.09);
                    float sTerm = dot(L, V) - nl * NdotV;
                    float tTerm = sTerm <= 0.0 ? 1.0 : max(max(nl, NdotV), 1e-4);
                    diffuseTerm = onA + onB * sTerm / tTerm;
                }
                // Same convention as the punctual path: no 1/PI, no kD, and no
                // explicit 1/(4 NdotL NdotV) because the visibility term carries it.
                vec3 clusteredSpecular = D * Vis * F * specularOn;
                color += (diffuseAlbedo * diffuseTerm + clusteredSpecular) * radiance * nl;
                directSpecular += clusteredSpecular * radiance * nl;
                directDiffuse += diffuseAlbedo * diffuseTerm * radiance * nl;
                bakeDiffuseLight += diffuseTerm * radiance * nl;
                bakeDirectLight += diffuseTerm * radiance * nl;
                if (vtFeatureEnabled(VT_FEATURE_CLEARCOAT_BIT)) {
                    // Nothing from a light behind the SURFACE, the coat included, as
                    // Metal skips the whole light there; every other term here already
                    // carries the surface's NdotL.
                    float ccNdotL = max(dot(ccNormalW, L), 0.0);
                    if (nl > 0.0 && ccNdotL > 0.0) {
                        float ccNdotH = max(dot(ccNormalW, H), 0.0);
                        float ccLdotH = max(dot(L, H), 0.0);
                        float ccDenom = ccNdotH * ccNdotH * (ccAlpha2 - 1.0) + 1.0;
                        float ccD = ccAlpha2 / max(PI * ccDenom * ccDenom, 1e-7);
                        ccSpecularLight += radiance * ccNdotL * ccD *
                            getVisibilityKelemen(ccLdotH) * getFresnelCC(ccLdotH);
                    }
                }
                if (vtFeatureEnabled(VT_FEATURE_SHEEN_BIT)) {
                    sheenSpecularDirect += radiance * nl * sheenTint
                        * sheenDistribution(nh, sheenRoughness)
                        * sheenVisibility(NdotV, nl);
                }
            }
        }
    }

