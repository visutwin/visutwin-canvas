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

                // Clustered spot shadow: each shadow-casting light owns one
                // slice of the atlas. shadowData = {castShadows, normalOffsetBias,
                // intensity, slice}. Mirrors forward-fragment-clustered.metal:
                // a receiver normal offset and an intensity blend, and NO depth
                // bias — the atlas pass biases on render, and this projection's
                // depth is far too crushed for a shader bias to be harmless.
                if (cl.shadowData.x > 0.5) {
                    vec3 shadowPosW = fragWorldPos + N * cl.shadowData.y;
                    vec4 sc = cl.shadowMatrix * vec4(shadowPosW, 1.0);
                    if (sc.w > 0.0) {
                        vec3 scoord = sc.xyz / sc.w;
                        if (all(greaterThanEqual(scoord, vec3(0.0))) &&
                            all(lessThanEqual(scoord, vec3(1.0)))) {
                            float vis = pcf3x3Array(scoord.xy, cl.shadowData.w, scoord.z);
                            atten *= mix(1.0, vis, clamp(cl.shadowData.z, 0.0, 1.0));
                        }
                    }
                }

                float nl = max(dot(N, L), 0.0);
                vec3 H = normalize(L + V);
                float nh = max(dot(N, H), 0.0);
                float vh = max(dot(V, H), 0.0);
                float D = distributionGGX(nh, roughness);
                float G = geometrySmith(NdotV, nl, roughness);
                vec3 F = fresnelSchlick(vh, F0);
                vec3 radiance = cl.colorIntensity.rgb *
                    cl.colorIntensity.w * atten;
                // Same convention as the punctual path: no 1/PI, no kD.
                color += (diffuseAlbedo +
                    D * G * F / max(4.0 * NdotV * nl, 1e-4)) *
                    radiance * nl;
                bakeDiffuseLight += radiance * nl;
                bakeDirectLight += radiance * nl;
            }
        }
    }

