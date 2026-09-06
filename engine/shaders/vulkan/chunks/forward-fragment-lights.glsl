    uint count = min(lighting.lightCount.x, 8u);
    for (uint i = 0u; i < count; ++i) {
        Light light = lighting.lights[i];
        uint type = uint(light.directionType.w + 0.5);

        vec3 L;
        float atten = 1.0;
        // Light cookie: the projected texture masking this light's color. Folded
        // into the radiance below, BEFORE any falloff (upstream lightFunctionLight.js).
        vec3 cookieMask = vec3(1.0);
        if (type == 0u) {
            L = normalize(-light.directionType.xyz);
            // Directional light is the CSM shadow caster.
            atten = sampleDirectionalShadow(fragWorldPos, fragViewDepth, N, L);
            // Parallax self-shadowing: the height field casts onto itself, which
            // the cascade map cannot see because it only knows the flat polygon.
            // Only the directional light pays for the extra march.
            if (parallaxShadowActive) {
                vec3 lightDirTS = normalize(vec3(
                    dot(parallaxTangent, L), dot(parallaxBitangent, L),
                    dot(parallaxNormalGeom, L)));
                atten *= parallaxSelfShadow(parallaxUv, lightDirTS,
                    material.heightMapFactor,
                    clamp(material.heightMapParams.x, 0.0, 1.0),
                    parallaxSurfaceDepth, material.heightMapParams.y);
            }
            // Shadow catcher only tracks directional shadows, and accumulates
            // here — before the NdotL early-out below — so back-facing texels
            // still report the shadow they receive (parity with
            // forward-fragment-lights.metal, which does the same).
            if (vtFeatureEnabled(VT_FEATURE_SHADOW_CATCHER_BIT)) {
                dShadowCatcher *= atten;
            }
        } else if (type == 3u && vtFeatureEnabled(VT_FEATURE_AREA_LIGHTS_BIT)) {
            // LTC area light (upstream ltc.js): diffuse and specular are both
            // LTC integrals, so this accumulates in full and skips the shared
            // punctual GGX below. Shape (0=rect, 1=disk, 2=sphere) rides in
            // coneParams.w — area lights never cast shadows, so that slot is
            // free, mirroring how the Metal path reuses its shadow slot.
            uint areaShape = uint(max(light.coneParams.w, 0.0) + 0.5);
            vec3 lightPos = light.positionRange.xyz;
            float halfW = light.areaRightHalfWidth.w;
            float halfH = light.areaUpHalfHeight.w;
            vec3 right = normalize(light.areaRightHalfWidth.xyz);
            vec3 up = normalize(light.areaUpHalfHeight.xyz);
            vec3 halfWidthVec = right * halfW;
            vec3 halfHeightVec = up * halfH;
            float sphereRadius = max(halfW, halfH);
            if (areaShape == 2u) {
                // Sphere: billboard the quad toward the reflection vector so the
                // disk math can integrate it (upstream getSphereLightCoords).
                vec3 f = reflect(
                    normalize(lightPos - lighting.cameraPosExposure.xyz), N);
                right = normalize(cross(f, halfHeightVec));
                up = normalize(cross(f, right));
                halfWidthVec = right * sphereRadius;
                halfHeightVec = up * sphereRadius;
            }

            // Corners, ccw (upstream getLTCLightCoords).
            vec3 p0 = lightPos + halfWidthVec - halfHeightVec;
            vec3 p1 = lightPos - halfWidthVec - halfHeightVec;
            vec3 p2 = lightPos - halfWidthVec + halfHeightVec;
            vec3 p3 = lightPos + halfWidthVec + halfHeightVec;

            vec3 toLight = lightPos - fragWorldPos;
            float areaAtten = ltcFalloffWindow(light.positionRange.w, toLight);
            if (areaAtten < 0.00001) {
                continue;
            }
            vec3 areaRadiance =
                light.colorIntensity.rgb * light.colorIntensity.w * areaAtten;

            // LUT2: Fresnel magnitude (x) + geometric attenuation (y), for
            // specular energy conservation.
            vec2 lutUv = ltcUv(N, V, roughness);
            vec4 t2 = textureLod(areaLightLut2, lutUv, 0.0);
            vec3 specFres = F0 * t2.x + (vec3(1.0) - F0) * t2.y;

            // Diffuse: LTC with the identity transform (plain cosine integral).
            // 16.0 mirrors the constant baked into the punctual inverse-square
            // falloff, so area and punctual lights of equal intensity are
            // comparably bright.
            mat3 ltcIdentity = mat3(1.0);
            float ltcDiffuse;
            if (areaShape == 1u) {
                ltcDiffuse = ltcEvaluateDisk(N, V, fragWorldPos, ltcIdentity,
                    p0, p1, p2);
            } else if (areaShape == 2u) {
                // Sphere diffuse: wrap-style Lambert with a radius-based
                // falloff (upstream getSphereLightDiffuse).
                float distSq = dot(toLight, toLight);
                float falloff = sphereRadius / (distSq + sphereRadius);
                ltcDiffuse = max(dot(N, normalize(toLight)), 0.0) * falloff;
            } else {
                ltcDiffuse = ltcEvaluateRect(N, V, fragWorldPos, ltcIdentity,
                    p0, p1, p2, p3);
            }
            vec3 areaDiffuse = diffuseAlbedo * areaRadiance * ltcDiffuse * 16.0 *
                (vec3(1.0) - specFres);
            color += areaDiffuse;
            directDiffuse += areaDiffuse;

            // Specular: LTC with the inverse transform from LUT1 (the sphere
            // uses the disk evaluator on its billboarded quad).
            vec4 t1 = textureLod(areaLightLut1, lutUv, 0.0);
            mat3 ltcMInv = mat3(
                vec3(t1.x, 0.0, t1.y),
                vec3(0.0, 1.0, 0.0),
                vec3(t1.z, 0.0, t1.w));
            float ltcSpec = (areaShape != 0u)
                ? ltcEvaluateDisk(N, V, fragWorldPos, ltcMInv, p0, p1, p2)
                : ltcEvaluateRect(N, V, fragWorldPos, ltcMInv, p0, p1, p2, p3);
            color += areaRadiance * ltcSpec * specFres;
            directSpecular += areaRadiance * ltcSpec * specFres;

            if (vtFeatureEnabled(VT_FEATURE_CLEARCOAT_BIT)) {
                // Clearcoat LTC specular with a fixed F0 of 0.04.
                float ccRough = clamp(material.clearCoatRoughness, 0.04, 1.0);
                vec2 ccUv = ltcUv(N, V, ccRough);
                vec4 ccT2 = textureLod(areaLightLut2, ccUv, 0.0);
                vec3 ccFres = vec3(0.04) * ccT2.x + vec3(0.96) * ccT2.y;
                vec4 ccT1 = textureLod(areaLightLut1, ccUv, 0.0);
                mat3 ccMInv = mat3(
                    vec3(ccT1.x, 0.0, ccT1.y),
                    vec3(0.0, 1.0, 0.0),
                    vec3(ccT1.z, 0.0, ccT1.w));
                float ccLtc = (areaShape != 0u)
                    ? ltcEvaluateDisk(N, V, fragWorldPos, ccMInv, p0, p1, p2)
                    : ltcEvaluateRect(N, V, fragWorldPos, ccMInv, p0, p1, p2, p3);
                color += material.clearCoatFactor * areaRadiance * ccLtc * ccFres;
            }

            // Fully accumulated — skip the shared punctual path.
            // DEVIATION: area lights neither cast nor receive shadows.
            continue;
        } else {
            vec3 lightPosition = light.positionRange.xyz;
            if (type == 3u) {
                // Area light without the LTC feature: approximate it as a
                // punctual light at the closest point on the rect, so it still
                // renders plausibly rather than disappearing.
                vec3 relative = fragWorldPos - lightPosition;
                float rightOffset = clamp(dot(relative,
                    light.areaRightHalfWidth.xyz),
                    -light.areaRightHalfWidth.w, light.areaRightHalfWidth.w);
                float upOffset = clamp(dot(relative,
                    light.areaUpHalfHeight.xyz),
                    -light.areaUpHalfHeight.w, light.areaUpHalfHeight.w);
                lightPosition += light.areaRightHalfWidth.xyz * rightOffset +
                    light.areaUpHalfHeight.xyz * upOffset;
            }
            vec3 toLight = lightPosition - fragWorldPos;
            float dist = length(toLight);
            L = (dist > 1e-4) ? toLight / dist : vec3(0.0, 1.0, 0.0);
            atten = distanceAttenuation(dist, light.positionRange.w, light.coneParams.z);

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
                    // Upstream samples by the light→fragment direction, the
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
                if (type == 1u) {
                    atten *= sampleOmniShadow(slot, fragWorldPos, light.positionRange.xyz);
                } else if (type == 2u) {
                    atten *= sampleSpotShadow(slot, fragWorldPos, N, L);
                }
            }
        }

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0 || atten <= 0.0) {
            continue;
        }

        vec3 H = normalize(L + V);
        float NdotH = max(dot(N, H), 0.0);
        float VdotH = max(dot(V, H), 0.0);

        float D = distributionGGX(NdotH, roughness);
        // Height-correlated visibility folds in 1/(4 NdotL NdotV), so there is no
        // explicit division here — see common-brdf.glsl.
        float Vis = getVisibilitySmithGGX(NdotV, NdotL, roughness);
        // Directional lights take the gloss-aware Fresnel; punctual lights take bare
        // specularity, as upstream's lightFunctionLight.js gates it and the Metal
        // chunk does. Applying the Fresnel to every light type over-brightened the
        // rim of everything lit by a point or spot light on this backend.
        vec3 F = (type == 0u) ? getFresnel(VdotH, 1.0 - roughness, F0) : F0;

        vec3 specular = D * Vis * F;

        vec3 radiance = light.colorIntensity.rgb * cookieMask * light.colorIntensity.w * atten;
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
        // energy-conservation factor. Upstream's lightDiffuseLambert is a bare
        // NdotL and its combine multiplies by albedo, and the Metal chunk matches
        // it; this divided by PI and multiplied by kD = (1 - F)(1 - metallic),
        // which made every direct light here about a third of Metal's. kD also
        // applied (1 - metallic) a second time, since diffuseAlbedo carries it.
        color += (diffuseAlbedo * diffuseTerm + specular) * radiance * NdotL;
        directDiffuse += diffuseAlbedo * diffuseTerm * radiance * NdotL;
        bakeDiffuseLight += diffuseTerm * radiance * NdotL;
        bakeDirectLight += diffuseTerm * radiance * NdotL;
        directSpecular += specular * radiance * NdotL;
        if (vtFeatureEnabled(VT_FEATURE_CLEARCOAT_BIT)) {
            // Twin of the clearcoat block in forward-fragment-lights.metal: GGX
            // distribution, Kelemen visibility (a coat is smooth enough that
            // Smith-GGX is not worth its cost) and a fixed F0 = 0.04 Fresnel, all
            // taken at the clearcoat's own half vector. This used to divide by
            // 4*NdotV with no NdotL anywhere, which is not a reflectance integral
            // at all — the coat brightened as the surface turned away from the light.
            float ccRough = clamp(material.clearCoatRoughness, 0.04, 1.0);
            float ccLdotH = max(dot(L, H), 0.0);
            float ccA = ccRough * ccRough;
            float ccA2 = ccA * ccA;
            float ccDenom = NdotH * NdotH * (ccA2 - 1.0) + 1.0;
            float ccD = ccA2 / max(PI * ccDenom * ccDenom, 1e-7);
            float ccVis = getVisibilityKelemen(ccLdotH);
            float ccF = getFresnelCC(ccLdotH);
            color += material.clearCoatFactor * radiance * NdotL * ccD * ccVis * ccF;
        }
        if (vtFeatureEnabled(VT_FEATURE_SHEEN_BIT)) {
            float velvet = pow(1.0 - max(NdotH, 0.0),
                mix(2.0, 8.0, material.sheenColor.w));
            color += material.sheenColor.rgb * velvet * radiance * NdotL;
        }
    }

