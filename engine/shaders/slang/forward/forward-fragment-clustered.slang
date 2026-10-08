// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
    if (vtFeatureEnabled(VT_FEATURE_LIGHT_CLUSTERING_BIT)) {
        int3 cell = int3(floor((fragWorldPos -
            lighting.clusterBoundsMin.xyz) *
            lighting.clusterCellsCountByBoundsSize.xyz));
        int3 dims = int3(lighting.clusterParams.xyz);
        if (all((cell >= int3(0))) &&
            all((cell < dims))) {
            uint maxPerCell = lighting.clusterParams.w;
            uint base = (uint(cell.y) * uint(dims.x) * uint(dims.z) +
                uint(cell.z) * uint(dims.x) + uint(cell.x)) * maxPerCell;
            for (uint slot = 0u; slot < maxPerCell; ++slot) {
                uint index1 = uint(clusterCells[base + slot]);
                if (index1 == 0u) break;
                ClusterLight cl = clusterLights[index1 - 1u];
                // The light's mask bits (areaHalfHeight.w: 1 affects dynamic meshes, 2
                // lightmapped ones) against the bit this draw accepts, from its
                // mesh-instance mask.
                if ((uint(cl.areaHalfHeight.w + 0.5) & lighting.clusterParams2.y) == 0u) continue;
                float3 delta = cl.positionRange.xyz - fragWorldPos;
                float distance = length(delta);
                float3 L = delta / max(distance, 1e-5);
                // A shaped light (areaHalfWidth.w, the LightShape; 0 punctual — the CPU
                // leaves it 0 unless clustered area lights are enabled) takes only the
                // range window.
                uint clShape = vtFeatureEnabled(VT_FEATURE_AREA_LIGHTS_BIT)
                    ? uint(cl.areaHalfWidth.w + 0.5) : 0u;
                float atten = (clShape != 0u)
                    ? ltcFalloffWindow(cl.positionRange.w, delta)
                    : distanceAttenuation(distance, cl.positionRange.w, cl.params.z);
                if (cl.params.y > 0.5) {
                    float cone = dot(normalize(-cl.directionSpot.xyz), L);
                    atten *= getSpotEffect(cl.params.x, cl.directionSpot.w, cone);
                }
                if (atten < 1e-5) continue;

                // Clustered shadow: each shadow-casting light owns a rect of the
                // atlas. shadowData = {castShadows, normalOffsetBias, intensity,
                // 1 spot / 2 omni}: a receiver normal offset and an intensity
                // blend; a spot projects through its viewport-aware VP with NO depth
                // bias (the atlas pass biases on render, and this projection's depth
                // is far too crushed for a shader bias to be harmless), an omni picks
                // a cube face from the direction and takes the cubemap path's
                // relative bias.
                if (cl.shadowData.x > 0.5) {
                    if (cl.shadowData.w > 1.5) {
                        // Omni receiver offset: geometric normal * normalBias *
                        // (1 - NdotL) * distance to the light.
                        float3 Ng = normalize(fragWorldNormal);
                        if (dot(Ng, N) < 0.0) Ng = -Ng;
                        float grazing = clamp(1.0 - dot(Ng, L), 0.0, 1.0);
                        float3 shadowPosW = fragWorldPos + Ng * (cl.shadowData.y * grazing * distance);
                        float vis = getShadowOmniClusteredPCF3(cl.shadowMatrix[0], cl.shadowMatrix[1],
                            shadowPosW - cl.positionRange.xyz);
                        atten *= lerp(1.0, vis, clamp(cl.shadowData.z, 0.0, 1.0));
                    } else {
                        // A spot keeps the flat normalBias.
                        float3 shadowPosW = fragWorldPos + N * cl.shadowData.y;
                        float4 sc = mul(float4(shadowPosW, 1.0), cl.shadowMatrix);
                        if (sc.w > 0.0) {
                            float3 scoord = sc.xyz / sc.w;
                            if (all((scoord >= float3(0.0))) &&
                                all((scoord <= float3(1.0)))) {
                                float vis = pcf3x3Atlas(scoord.xy, scoord.z);
                                atten *= lerp(1.0, vis, clamp(cl.shadowData.z, 0.0, 1.0));
                            }
                        }
                    }
                }

                // Clustered cookie: params.w is 0 for none, else
                // (CookieChannel + 1) * 2 + intensity; read through the shadow's own
                // projection (spot) or face pick (omni), in the cookie atlas.
                float3 cookieMask = float3(1.0);
                if (cl.params.w > 0.0) {
                    float channelCode = floor(cl.params.w * 0.5);
                    uint cookieChannel = uint(channelCode - 1.0);
                    float cookieIntensity = cl.params.w - channelCode * 2.0;
                    float2 cookieUv;
                    if (cl.shadowData.w > 1.5) {
                        cookieUv = getCubemapAtlasCoordinates(cl.shadowMatrix[0],
                            fragWorldPos - cl.positionRange.xyz);
                    } else {
                        float4 projected = mul(float4(fragWorldPos, 1.0), cl.shadowMatrix);
                        cookieUv = projected.xy / max(projected.w, 1e-6);
                    }
                    float4 texel = clusterCookieAtlas.SampleLevel(linearClampSampler, cookieUv, 0.0);
                    float3 channelValue = texel.rgb;
                    if (cookieChannel == 1u) channelValue = float3(texel.r);
                    else if (cookieChannel == 2u) channelValue = float3(texel.g);
                    else if (cookieChannel == 3u) channelValue = float3(texel.b);
                    else if (cookieChannel == 4u) channelValue = float3(texel.a);
                    cookieMask = lerp(float3(1.0), channelValue, cookieIntensity);
                }

                if (clShape != 0u) {
                    // An area light: LTC, as the main loop (common-ltc's helpers).
                    float3 p0, p1, p2, p3;
                    float sphereRadius = ltcAreaLight(clShape, cl.positionRange.xyz, cl.areaHalfWidth.xyz,
                        cl.areaHalfHeight.xyz, N, lighting.cameraPosExposure.xyz, p0, p1, p2, p3);
                    float3 areaRadiance = cl.colorIntensity.rgb * cl.colorIntensity.w * atten * cookieMask;
                    float3 specFres = ltcSpecularFresnel(N, V, roughness, F0);
                    float3 areaDiffuse = diffuseAlbedo * areaRadiance * (float3(1.0) - specFres) *
                        ltcAreaDiffuse(clShape, sphereRadius, cl.positionRange.xyz, N, V, fragWorldPos,
                            p0, p1, p2, p3);
                    float3 areaSpecular = areaRadiance * specFres * specularOn *
                        ltcAreaSpecular(clShape, N, V, fragWorldPos, roughness, p0, p1, p2, p3);
                    color += areaDiffuse + areaSpecular;
                    directDiffuse += areaDiffuse;
                    directSpecular += areaSpecular;
                    if (vtFeatureEnabled(VT_FEATURE_CLEARCOAT_BIT)) {
                        ccSpecularLight += areaRadiance *
                            ltcSpecularFresnel(ccNormalW, V, 1.0 - ccGlossiness, float3(0.04)) *
                            ltcAreaSpecular(clShape, ccNormalW, V, fragWorldPos, 1.0 - ccGlossiness,
                                p0, p1, p2, p3);
                    }
                    continue;
                }

                float nl = max(dot(N, L), 0.0);
                float3 H = normalize(L + V);
                float nh = max(dot(N, H), 0.0);
                float vh = max(dot(V, H), 0.0);
                // Same BRDF as the non-clustered loop (common-brdf.slang). Clustered
                // lights are punctual, so the Fresnel is bare specularity — the
                // gloss-aware curve is the directional case only.
                float D = distributionGGX(nh, roughness);
                float Vis = getVisibilitySmithGGX(NdotV, nl, roughness);
                if (vtFeatureEnabled(VT_FEATURE_ANISOTROPY_BIT)) {
                    // Anisotropic GGX (common-brdf): D carries the whole D * Vis product.
                    D = getLightSpecularAnisoGGX(N, V, H, L, anisoT, anisoB, anisoAlpha);
                    Vis = 1.0;
                }
                float3 F = F0;
                if (vtFeatureEnabled(VT_FEATURE_IRIDESCENCE_BIT)) {
                    F = lerp(F, iridFresnel, iridIntensity);
                }
                float3 radiance = cl.colorIntensity.rgb *
                    cl.colorIntensity.w * atten * cookieMask;
                // Oren-Nayar, iridescence, clearcoat and sheen below are the main
                // loop's terms (forward-fragment-lights.slang). With clustered
                // lighting the default, this is the loop EVERY spot and omni light
                // goes through, so a term missing here is missing under every local
                // light.
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
                float3 clusteredSpecular = D * Vis * F * specularOn;
                // With area lights in the variant, a punctual light's diffuse
                // is scaled by (1 - specularity).
                float3 clDiffuseScale = (vtFeatureEnabled(VT_FEATURE_AREA_LIGHTS_BIT) && specularOn > 0.0)
                    ? float3(1.0) - F0 : float3(1.0);
                color += (diffuseAlbedo * diffuseTerm * clDiffuseScale + clusteredSpecular) * radiance * nl;
                directSpecular += clusteredSpecular * radiance * nl;
                directDiffuse += diffuseAlbedo * diffuseTerm * clDiffuseScale * radiance * nl;
                bakeDiffuseLight += diffuseTerm * radiance * nl;
                bakeDirectLight += diffuseTerm * radiance * nl;
                if (vtFeatureEnabled(VT_FEATURE_CLEARCOAT_BIT)) {
                    // Nothing from a light behind the SURFACE, the coat included; every
                    // other term here already carries the surface's NdotL.
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

