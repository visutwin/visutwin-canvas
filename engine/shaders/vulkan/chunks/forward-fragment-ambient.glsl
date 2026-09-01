    // Shadow catcher REPLACES the shaded result with the accumulated shadow
    // factor as grayscale, so a ground plane can receive shadows from virtual
    // geometry over a real backdrop (AR compositing). Returns raw — deliberately
    // no fog, exposure, tonemap or gamma — because the value is a blend
    // coefficient, not a colour: with multiplicative blending (src * dst), white
    // leaves the framebuffer untouched and darker values darken it where shadows
    // fall. Encoding it would bend the shadow response.
    //
    // Placed here to mirror forward-fragment-tail.metal, which returns at the
    // top of the tail — after the light loop, before any indirect composition.
    if (vtFeatureEnabled(VT_FEATURE_SHADOW_CATCHER_BIT)) {
        outColor = vec4(vec3(dShadowCatcher), 1.0);
        return;
    }

    // Indirect lighting.  With an environment atlas: image-based diffuse
    // irradiance + roughness-prefiltered specular reflection.  Without one:
    // a flat ambient term plus a Fresnel-weighted specular floor so metals
    // aren't pitch black.
    vec3 indirect;
    // Mirrors the specular part of the indirect contribution exactly as it lands
    // in `color` below, AO factor included. SSR replaces that term where the
    // reflection ray hits on-screen geometry, so it only has to add the
    // difference rather than restructure the accumulation.
    vec3 indirectSpecular = vec3(0.0);
    if (vtFeatureEnabled(VT_FEATURE_ENV_ATLAS_BIT) &&
        lighting.envParams.y > 0.5) {
        float intensity = max(lighting.envParams.x, 0.0);

        // Diffuse irradiance (the negate-X matches the engine's atlas lookup
        // handedness).
        vec3 diffDir = vec3(-N.x, N.y, N.z);
        vec3 irradiance = decodeEnv(texture(envAtlas, mapAmbientUv(dirToEquirect(diffDir)))) * intensity;

        // Specular: reflect, pick a roughness mip, trilinear between levels.
        vec3 R = reflect(-V, N);
        vec3 specDir = vec3(-R.x, R.y, R.z);
        vec2 envUv = dirToEquirect(specDir);
        float level = clamp(roughness * 5.0, 0.0, 5.0);
        float l0 = floor(level);
        vec3 envA = decodeEnv(texture(envAtlas, mapRoughnessUv(envUv, l0)));
        vec3 envB = decodeEnv(texture(envAtlas, mapRoughnessUv(envUv, l0 + 1.0)));
        vec3 prefiltered = mix(envA, envB, level - l0) * intensity;

        // Schlick-roughness Fresnel for the environment term.
        vec3 Fr = F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, 5.0);
        vec3 kD = (vec3(1.0) - Fr) * (1.0 - metallic);

        indirect = kD * irradiance * diffuseAlbedo + prefiltered * Fr;
        indirectSpecular = prefiltered * Fr;
        bakeDiffuseLight += kD * irradiance;
    } else if (vtFeatureEnabled(VT_FEATURE_LIGHT_PROBES_BIT)) {
        vec3 shN = N;
        vec3 irradiance =
            lighting.ambientSH[0].rgb +
            lighting.ambientSH[1].rgb * shN.x +
            lighting.ambientSH[2].rgb * shN.y +
            lighting.ambientSH[3].rgb * shN.z +
            lighting.ambientSH[4].rgb * (shN.x * shN.z) +
            lighting.ambientSH[5].rgb * (shN.z * shN.y) +
            lighting.ambientSH[6].rgb * (shN.y * shN.x) +
            lighting.ambientSH[7].rgb * (3.0 * shN.z * shN.z - 1.0) +
            lighting.ambientSH[8].rgb * (shN.x * shN.x - shN.y * shN.y);
        indirect = max(irradiance, vec3(0.0)) * diffuseAlbedo;
        bakeDiffuseLight += max(irradiance, vec3(0.0));
    } else {
        indirect = lighting.ambient.rgb * diffuseAlbedo + lighting.ambient.rgb * F0;
        indirectSpecular = lighting.ambient.rgb * F0;
        bakeDiffuseLight += lighting.ambient.rgb;
    }
    if (vtFeatureEnabled(VT_FEATURE_LIGHTMAP_BIT)) {
        // The bake REPLACES the ambient diffuse rather than adding to it — upstream
        // gates the ambient behind `addAmbient = !lightMapEnabled` (lit-shader.js),
        // so that a lightmapped surface is not lit twice by what the bake already
        // contains. Matches the Metal chunk (forward-fragment-tail).
        // Lightmaps store LINEAR light (see the bake output and Lightmapper's encoder).
        indirect = max(texture(lightMap, fragUV1).rgb, vec3(0.0)) * diffuseAlbedo;
    }
    color += indirect * ao;
    indirectSpecular *= ao;
    if (vtFeatureEnabled(VT_FEATURE_REFLECTION_PROBE_BIT)) {
        vec3 probeDirection = reflect(-V, N);
        if (lighting.reflectionProbeParams.x > 0.5) {
            vec3 rbmax = (lighting.reflectionProbeBoxMax.xyz - fragWorldPos) /
                max(abs(probeDirection), vec3(1e-5));
            vec3 rbmin = (lighting.reflectionProbeBoxMin.xyz - fragWorldPos) /
                max(abs(probeDirection), vec3(1e-5));
            vec3 distances = mix(rbmin, rbmax,
                greaterThan(probeDirection, vec3(0.0)));
            float distance = min(distances.x, min(distances.y, distances.z));
            probeDirection = fragWorldPos + probeDirection * distance -
                lighting.reflectionProbePosition.xyz;
        }
        vec3 probeSpecular = textureLod(reflectionProbeCube, probeDirection,
            roughness * lighting.reflectionProbeParams.z).rgb *
            F0 * lighting.reflectionProbeParams.y;
        color += probeSpecular;
        // The probe replaces the environment specular rather than adding to it.
        indirectSpecular = probeSpecular;
    }
    // Screen-space reflections: march the reflection ray against the scene depth
    // grab and sample the scene color grab at the hit, blending OVER the
    // probe/env-atlas specular where the ray lands on on-screen geometry.
    if (vtFeatureEnabled(VT_FEATURE_SSR_BIT) &&
        lighting.cameraNearFar.z > 0.5 && lighting.cameraNearFar.w > 0.5) {
        float ssrNear = lighting.cameraNearFar.x;
        float ssrFar = lighting.cameraNearFar.y;
        vec3 ssrR = reflect(-V, N);

        const int SSR_STEPS = 48;
        const float SSR_MAX_DIST = 60.0;
        float ssrStep = SSR_MAX_DIST / float(SSR_STEPS);
        const float ssrThickness = 1.5;   // view-space hit tolerance (world units)

        vec2 ssrHitUv = vec2(0.0);
        float ssrHit = 0.0;
        for (int i = 1; i <= SSR_STEPS; ++i) {
            vec3 samplePos = fragWorldPos + ssrR * (ssrStep * float(i));
            vec4 clip = lighting.viewProjection * vec4(samplePos, 1.0);
            if (clip.w <= 0.0) break;                     // behind the camera
            // Vulkan clip space is already Y-down, so NDC maps straight to UV
            // (the Metal path flips Y here).
            vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
            if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break;
            float marchedZ = clip.w;                      // view-space distance
            float rawDepth = texture(ssrSceneDepth, uv).r;
            float sceneZ = (ssrNear * ssrFar) /
                max(ssrFar - rawDepth * (ssrFar - ssrNear), 1e-6);
            float diff = marchedZ - sceneZ;               // >0 = behind the surface
            if (diff > 0.0 && diff < ssrThickness) {
                ssrHitUv = uv;
                ssrHit = 1.0;
                break;
            }
        }

        if (ssrHit > 0.0) {
            // The Vulkan forward pass always tonemaps and gamma-encodes, so the
            // grab is display-encoded unconditionally (the Metal path has to
            // check its HDR camera-frame flag before decoding).
            vec3 ssrColor = srgbToLinear(texture(ssrSceneColor, ssrHitUv).rgb);
            // Fade at screen edges (reflections pop as rays exit the frame) and
            // on rough surfaces (this port marches sharp — no roughness cone).
            vec2 eLo = smoothstep(vec2(0.0), vec2(0.12), ssrHitUv);
            vec2 eHi = 1.0 - smoothstep(vec2(0.88), vec2(1.0), ssrHitUv);
            float edgeFade = eLo.x * eLo.y * eHi.x * eHi.y;
            float gloss = 1.0 - roughness;
            float roughFade = clamp(gloss * 1.2 - 0.2, 0.0, 1.0);
            vec3 ssrFres = ssrFresnel(NdotV, gloss, F0);
            vec3 replaced = mix(indirectSpecular, ssrColor * ssrFres,
                edgeFade * roughFade);
            color += replaced - indirectSpecular;
            indirectSpecular = replaced;
        }
    }
    if (vtFeatureEnabled(VT_FEATURE_TRANSMISSION_BIT)) {
        if (vtFeatureEnabled(VT_FEATURE_DYNAMIC_REFRACTION_BIT) &&
            lighting.cameraNearFar.z > 0.5 && material.transmissionFactor > 0.0) {
            // Dynamic grab-pass refraction (upstream refractionDynamic.js):
            // sample the mid-frame scene colour grab at the screen position of
            // the refracted exit point instead of the environment atlas.
            float ior = max(material.refractionIndex, 1.001);
            float thickness = max(material.thickness, 0.0);

            // Dispersion (KHR_materials_dispersion): spread the refraction eta
            // per channel and sample R/G/B separately.
            float dispersion = max(material.dispersionParams.x, 0.0);
            float eta = 1.0 / ior;
            float halfSpread = (ior - 1.0) * 0.025 * dispersion;
            int refrSamples = (dispersion > 0.0) ? 3 : 1;

            // Mip range of the grab chain; higher IOR and rougher surfaces read
            // blurrier scene colour (upstream iorToRoughness).
            float grabMips =
                log2(max(float(textureSize(ssrSceneColor, 0).x), 2.0));
            float gloss = 1.0 - roughness;

            vec3 refrColor = vec3(0.0);
            for (int ch = 0; ch < refrSamples; ++ch) {
                float etaCh = (refrSamples == 1)
                    ? eta : (eta + halfSpread * float(ch - 1));
                vec3 refrDir = refract(-V, N, etaCh);

                // Refraction vector scaled by volume thickness; total internal
                // reflection falls back to the unshifted surface point.
                // DEVIATION: upstream scales by the model matrix' per-axis
                // scale, unavailable here, so thickness is in world units.
                vec3 refractionVector = (dot(refrDir, refrDir) > 0.0)
                    ? normalize(refrDir) * thickness : vec3(0.0);

                vec4 projected = lighting.viewProjection *
                    vec4(fragWorldPos + refractionVector, 1.0);
                float invW = 1.0 / max(projected.w, 1e-6);
                // No Y flip: Vulkan clip space is already Y-down.
                vec2 grabUv = clamp(projected.xy * invW * 0.5 + 0.5, 0.001, 0.999);

                float iorCh = 1.0 / etaCh;
                float iorToRoughness = clamp(1.0 - gloss, 0.0, 1.0) *
                    clamp(iorCh * 2.0 - 2.0, 0.0, 1.0);
                float refractionLod = grabMips * iorToRoughness;
                // The Vulkan forward pass always tonemaps, so the grab is
                // display-encoded unconditionally.
                vec3 sampleColor = srgbToLinear(
                    textureLod(ssrSceneColor, grabUv, refractionLod).rgb);
                if (refrSamples == 1) {
                    refrColor = sampleColor;
                } else {
                    refrColor[ch] = sampleColor[ch];
                }
            }

            // Volume transmittance (KHR_materials_volume Beer's law). Distance 0
            // keeps the legacy baseColor^thickness tint.
            if (material.attenuationParams.w > 0.0) {
                vec3 attColor = clamp(material.attenuationParams.rgb, 0.0001, 1.0);
                refrColor *= exp(-(-log(attColor) / material.attenuationParams.w) *
                    thickness);
            } else {
                refrColor *= pow(max(albedo.rgb, vec3(0.0)), vec3(thickness + 1.0));
            }

            // Fresnel: grazing angles reflect more, normal incidence transmits.
            float F0ior = pow((1.0 - ior) / (1.0 + ior), 2.0);
            float fresnel = F0ior + (1.0 - F0ior) * pow(1.0 - NdotV, 5.0);
            float transmission = material.transmissionFactor * (1.0 - fresnel);

            // Replace surface diffuse with the refracted scene, keep specular.
            // Emissive is added after this block, so it survives on its own.
            vec3 specPart = directSpecular + indirectSpecular;
            color = mix(color, refrColor + specPart, clamp(transmission, 0.0, 1.0));
        } else {
            float transmission = clamp(material.transmissionFactor, 0.0, 1.0);
            vec3 transmitted = indirect;
            if (material.attenuationParams.w > 0.0) {
                // Same Beer's law as the dynamic path: a^(t/d) == exp(-(-ln a/d)*t).
                transmitted *= pow(max(material.attenuationParams.rgb, vec3(1e-4)),
                    vec3(material.thickness / material.attenuationParams.w));
            }
            color = mix(color, transmitted, transmission);
        }
    }

