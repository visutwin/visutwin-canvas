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

    // Indirect lighting, split the way upstream and the Metal chunk split it:
    // the DIFFUSE irradiance comes from the first of SH light probes, the
    // environment atlas' ambient rect, or the flat ambient; the SPECULAR
    // reflection comes from the environment atlas whenever it is bound, whatever
    // supplied the diffuse. Until 2026-09-19 this backend put the probes and the
    // atlas in ONE if/else-if, so a scene carrying both lost every environment
    // reflection the moment its probes were enabled — the metals went flat —
    // while Metal kept them. Diffuse and specular are kept apart so ambient
    // occlusion can treat them as upstream does (see the occlusion block below).
    vec3 ambientIrradiance;
    if (vtFeatureEnabled(VT_FEATURE_LIGHT_PROBES_BIT)) {
        // 9-coefficient irradiance in the world normal direction (upstream
        // AMBIENTSH basis, coefficients premultiplied).
        vec3 shN = N;
        ambientIrradiance = max(
            lighting.ambientSH[0].rgb +
            lighting.ambientSH[1].rgb * shN.x +
            lighting.ambientSH[2].rgb * shN.y +
            lighting.ambientSH[3].rgb * shN.z +
            lighting.ambientSH[4].rgb * (shN.x * shN.z) +
            lighting.ambientSH[5].rgb * (shN.z * shN.y) +
            lighting.ambientSH[6].rgb * (shN.y * shN.x) +
            lighting.ambientSH[7].rgb * (3.0 * shN.z * shN.z - 1.0) +
            lighting.ambientSH[8].rgb * (shN.x * shN.x - shN.y * shN.y),
            vec3(0.0));
    } else {
        ambientIrradiance = lighting.ambient.rgb;
    }
    // Mirrors the specular part of the indirect contribution exactly as it lands
    // in `color` below, AO factor included. SSR replaces that term where the
    // reflection ray hits on-screen geometry, so it only has to add the
    // difference rather than restructure the accumulation.
    vec3 indirectSpecular = vec3(0.0);
    if (vtFeatureEnabled(VT_FEATURE_ENV_ATLAS_BIT) &&
        lighting.envParams.y > 0.5 &&
        // bit 18: useSkybox off — upstream's useSceneEnv, so this material keeps
        // the probe or flat ambient above and takes no scene reflection.
        (material.flags & (1u << 18)) == 0u) {
        float intensity = max(lighting.envParams.x, 0.0);

        if (!vtFeatureEnabled(VT_FEATURE_LIGHT_PROBES_BIT)) {
            // Diffuse irradiance from the atlas' Lambert rect (the negate-X matches
            // the engine's atlas lookup handedness). Probes take priority over it,
            // as on Metal.
            vec3 diffDir = vec3(-N.x, N.y, N.z);
            ambientIrradiance = decodeEnv(texture(envAtlas, mapAmbientUv(dirToEquirect(diffDir)))) * intensity;
        }

        // Specular: reflect, pick a mip, trilinear between levels. Twin of the
        // block in forward-fragment-ambient.metal, including its shiny path.
        // Anisotropic materials bend the normal first (upstream reflDirAniso,
        // common-brdf), as the Metal chunk does.
        vec3 R = vtFeatureEnabled(VT_FEATURE_ANISOTROPY_BIT)
            ? getReflDirAniso(N, V, anisoB, 1.0 - roughness, anisoIntensity)
            : reflect(-V, N);
        vec3 specDir = vec3(-R.x, R.y, R.z);
        vec2 envUv = dirToEquirect(specDir);
        float level = clamp(roughness * 5.0, 0.0, 5.0);
        float l0 = floor(level);

        // Screen-space mip for the sharp rect (upstream shinyMipLevel). The second
        // derivative pair is taken on fract(u + 0.5) so the azimuthal wrap, where u
        // jumps 1 -> 0 across one pixel, does not read as an enormous gradient and
        // force the blurriest mip along that seam.
        vec2 shinyUvFull = envUv * ATLAS_SIZE;
        vec2 dxA = dFdx(shinyUvFull);
        vec2 dyA = dFdy(shinyUvFull);
        vec2 uvWrapped = vec2(fract(envUv.x + 0.5), envUv.y) * ATLAS_SIZE;
        vec2 dxB = dFdx(uvWrapped);
        vec2 dyB = dFdy(uvWrapped);
        float maxd = min(max(dot(dxA, dxA), dot(dyA, dyA)),
                         max(dot(dxB, dxB), dot(dyB, dyB)));
        float shinyLevel = clamp(0.5 * log2(max(maxd, 1e-12)) - 1.0, 0.0, 5.0);
        float shinyL0 = floor(shinyLevel);

        // A mirror (level 0) takes the unconvolved shiny rect; anything rougher
        // takes the prefiltered chain, and both blend toward the next roughness mip.
        vec3 envA;
        if (l0 == 0.0) {
            vec3 shinyA = decodeEnv(texture(envAtlas, mapShinyUv(envUv, shinyL0)));
            vec3 shinyB = decodeEnv(texture(envAtlas, mapShinyUv(envUv, shinyL0 + 1.0)));
            envA = mix(shinyA, shinyB, shinyLevel - shinyL0);
        } else {
            envA = decodeEnv(texture(envAtlas, mapRoughnessUv(envUv, l0)));
        }
        vec3 envB = decodeEnv(texture(envAtlas, mapRoughnessUv(envUv, l0 + 1.0)));
        vec3 prefiltered = mix(envA, envB, level - l0) * intensity;

        // Gloss-aware Fresnel for the environment term — the same curve the Metal
        // chunk uses (getFresnel), and the same helper this file already uses for
        // SSR. The hand-rolled Schlick-roughness variant here returned up to
        // (1 - roughness) at grazing angles where this returns ~F0, so the
        // environment specular ran far hotter than Metal's.
        vec3 Fr = ssrFresnel(NdotV, 1.0 - roughness, F0);
        if (vtFeatureEnabled(VT_FEATURE_IRIDESCENCE_BIT)) {
            Fr = mix(Fr, iridFresnel, iridIntensity);
        }
        indirectSpecular = prefiltered * Fr * specularOn;
    }
    // No kD on the irradiance: it used to be scaled by (1 - Fr) * (1 - metallic),
    // which applied (1 - metallic) a SECOND time because diffuseAlbedo already
    // carries it — a 3.3x deficit on a material at metalness 0.7 — and no other
    // source here nor the Metal chunk does that. No specular floor without an
    // atlas either: `ambient * F0` is not a term Metal or upstream has, and it lit
    // metals from nothing in scenes with no environment.
    // upstream litForwardBackend.js, right after addAmbient: the ambient diffuse is
    // scaled by (1 - specularity) per channel when the material renders specular
    // (twin of the block in forward-fragment-ambient.metal, which explains it).
    ambientIrradiance *= mix(vec3(1.0), vec3(1.0) - F0, specularOn);
    vec3 indirectDiffuse = ambientIrradiance * diffuseAlbedo;
    bakeDiffuseLight += ambientIrradiance;
    // Sheen image-based lighting: sample the atlas along the reflection at the
    // sheen roughness, scaled by the analytical directional albedo instead of the
    // DFG lookup upstream samples. Twin of the block in
    // forward-fragment-ambient.metal; this backend had no sheen IBL at all, so a
    // sheened surface lit only by an environment showed nothing.
    if (vtFeatureEnabled(VT_FEATURE_SHEEN_BIT) &&
        vtFeatureEnabled(VT_FEATURE_ENV_ATLAS_BIT) && lighting.envParams.y > 0.5) {
        vec3 sheenR = reflect(-V, N);
        vec2 sheenEnvUv = dirToEquirect(normalize(vec3(-sheenR.x, sheenR.y, sheenR.z)));
        float sheenLevel = clamp(sheenRoughness, 0.0, 1.0) * 5.0;
        float sheenL0 = floor(sheenLevel);

        vec3 sheenEnvColor;
        if (sheenL0 == 0.0) {
            vec3 sa = decodeEnv(texture(envAtlas, mapShinyUv(sheenEnvUv, 0.0)));
            vec3 sb = decodeEnv(texture(envAtlas, mapShinyUv(sheenEnvUv, 1.0)));
            sheenEnvColor = mix(sa, sb, sheenLevel);
        } else {
            vec3 sa = decodeEnv(texture(envAtlas, mapRoughnessUv(sheenEnvUv, sheenL0)));
            vec3 sb = decodeEnv(texture(envAtlas, mapRoughnessUv(sheenEnvUv, sheenL0 + 1.0)));
            sheenEnvColor = mix(sa, sb, sheenLevel - sheenL0);
        }
        sheenSpecularIndirect = sheenEnvColor * max(lighting.envParams.x, 0.0)
            * sheenTint * sheenIBLApprox(max(dot(N, V), 0.001), sheenRoughness);
    }

    // Ambient occlusion on the AMBIENT diffuse: upstream (litForwardBackend.js)
    // runs occludeDiffuse before addLightMap and before the light loop, so the
    // bake and the direct light are occluded only under occludeDirect — handled
    // in the occlusion block after the specular terms are final.
    indirectDiffuse *= ao;
    if (vtFeatureEnabled(VT_FEATURE_LIGHTMAP_BIT)) {
        // The bake REPLACES the ambient diffuse rather than adding to it — upstream
        // gates the ambient behind `addAmbient = !lightMapEnabled` (lit-shader.js),
        // so that a lightmapped surface is not lit twice by what the bake already
        // contains. Matches the Metal chunk (forward-fragment-tail).
        // Lightmaps store LINEAR light (see the bake output and Lightmapper's encoder).
        indirectDiffuse = max(texture(lightMap, fragUV1).rgb, vec3(0.0)) * diffuseAlbedo;
    }
    color += indirectDiffuse + indirectSpecular;
    if (vtFeatureEnabled(VT_FEATURE_REFLECTION_PROBE_BIT)) {
        // Upstream samples every reflection source along the one (bent) dReflDirW.
        vec3 reflectDir = vtFeatureEnabled(VT_FEATURE_ANISOTROPY_BIT)
            ? getReflDirAniso(N, V, anisoB, 1.0 - roughness, anisoIntensity)
            : reflect(-V, N);
        vec3 sampleDir = reflectDir;

        // Box projection (upstream cubeMapProject BOX): intersect the reflection
        // ray with the probe box, then re-aim from the box CENTRE — that is what
        // makes a flat cubemap track a room's walls as the surface moves. Aiming
        // from the probe's own position instead, and leaving the result
        // unnormalised, pointed the lookup somewhere else entirely.
        if (lighting.reflectionProbeParams.x > 0.5) {
            vec3 boxMin = lighting.reflectionProbeBoxMin.xyz;
            vec3 boxMax = lighting.reflectionProbeBoxMax.xyz;
            vec3 invDir = 1.0 / reflectDir;
            vec3 rbmax = (boxMax - fragWorldPos) * invDir;
            vec3 rbmin = (boxMin - fragWorldPos) * invDir;
            vec3 rbminmax = mix(rbmin, rbmax, greaterThan(reflectDir, vec3(0.0)));
            float fa = min(min(rbminmax.x, rbminmax.y), rbminmax.z);
            vec3 posOnBox = fragWorldPos + reflectDir * fa;
            sampleDir = normalize(posOnBox - (boxMin + boxMax) * 0.5);
        }

        // The engine's cube convention flips X, the same as every sky path here.
        vec3 cubeDir = vec3(-sampleDir.x, sampleDir.y, sampleDir.z);
        float gloss = 1.0 - roughness;
        float probeLod = clamp(1.0 - gloss, 0.0, 1.0) * lighting.reflectionProbeParams.z;
        // The captured cube is written gamma-encoded, so it owes a decode before it
        // can be added to linear light — the same split the base-colour and
        // emissive maps follow. Without it the probe reads far too bright, which is
        // what washed every metallic surface here out to pale pastel.
        vec3 probeSpecular =
            pow(max(textureLod(reflectionProbeCube, cubeDir, probeLod).rgb, vec3(0.0)),
                vec3(2.2)) * lighting.reflectionProbeParams.y;

        // Gloss-aware Fresnel, not a raw F0 multiply (parity with the Metal chunk).
        vec3 probeFresnel = ssrFresnel(NdotV, gloss, F0);
        if (vtFeatureEnabled(VT_FEATURE_IRIDESCENCE_BIT)) {
            probeFresnel = mix(probeFresnel, iridFresnel, iridIntensity);
        }

        // The probe REPLACES the environment specular. `color` already carries that
        // term from the block above, so add only the difference — adding the probe
        // outright counted both.
        vec3 replaced = probeSpecular * probeFresnel * specularOn;
        color += replaced - indirectSpecular;
        indirectSpecular = replaced;
    }
    // Screen-space reflections: march the reflection ray against the scene depth
    // grab and sample the scene colour grab at the hit, blending OVER the
    // probe/env-atlas specular where the ray lands on on-screen geometry. Twin of
    // the block in forward-fragment-ambient.metal, which explains the explicit LOD,
    // the point-sampled depth (nearestClampSampler, see the head) and the bisection.
    if (vtFeatureEnabled(VT_FEATURE_SSR_BIT) &&
        lighting.cameraNearFar.z > 0.5 && lighting.cameraNearFar.w > 0.5) {
        float ssrNear = lighting.cameraNearFar.x;
        float ssrFar = lighting.cameraNearFar.y;
        vec3 ssrR = reflect(-V, N);

        const int SSR_STEPS = 48;
        const int SSR_REFINE = 6;
        float ssrMaxDist = 0.4 * (ssrFar - ssrNear);
        float ssrStep = ssrMaxDist / float(SSR_STEPS);
        float ssrThickness = ssrStep * 1.25;   // view-space tolerance behind the surface

        vec2 ssrHitUv = vec2(0.0);
        float ssrHit = 0.0;
        float ssrHitT = 0.0;   // distance along the ray to the hit
        float ssrHitW = 1.0;   // the hit's view depth
        float tPrev = 0.0;
        for (int i = 1; i <= SSR_STEPS; ++i) {
            float t = ssrStep * float(i);
            vec3 samplePos = fragWorldPos + ssrR * t;
            vec4 clip = lighting.viewProjection * vec4(samplePos, 1.0);
            if (clip.w <= 0.0) break;                     // behind the camera
            // The backend rasterises through a NEGATED-height viewport so that
            // projection matrices written for Metal work unchanged, which puts
            // NDC +Y at the TOP row of every target, back buffer and offscreen
            // alike. A projected point therefore maps to a texture coordinate
            // exactly as it does on Metal, Y included.
            vec2 uv = clip.xy / clip.w * vec2(0.5, -0.5) + 0.5;
            if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break;
            float rawDepth = textureLod(ssrSceneDepth, uv, 0.0).r;
            float sceneZ = (ssrNear * ssrFar) / max(ssrFar - rawDepth * (ssrFar - ssrNear), 1e-6);
            float diff = clip.w - sceneZ;                 // >0 = behind the surface
            if (diff > 0.0) {
                // Any crossing is a candidate; bisect to it and judge the thickness there
                // (see the Metal twin). A rejected silhouette jump does not end the march.
                float lo = tPrev, hi = t;
                vec2 hitUv = uv;
                float hitDiff = diff;
                float hitT = t, hitW = clip.w;
                for (int k = 0; k < SSR_REFINE; ++k) {
                    float mid = 0.5 * (lo + hi);
                    vec4 c = lighting.viewProjection * vec4(fragWorldPos + ssrR * mid, 1.0);
                    vec2 u = c.xy / c.w * vec2(0.5, -0.5) + 0.5;
                    float rd = textureLod(ssrSceneDepth, u, 0.0).r;
                    float d = c.w - (ssrNear * ssrFar) / max(ssrFar - rd * (ssrFar - ssrNear), 1e-6);
                    if (d > 0.0) { hi = mid; hitUv = u; hitDiff = d; hitT = mid; hitW = c.w; } else { lo = mid; }
                }
                if (hitDiff < ssrThickness) {
                    ssrHitUv = hitUv;
                    ssrHit = 1.0;
                    ssrHitT = hitT;
                    ssrHitW = hitW;
                    break;
                }
            }
            tPrev = t;
        }

        if (ssrHit > 0.0) {
            // The standalone grab copies the tonemapped, gamma-encoded back buffer
            // and owes a decode. Under the camera-frame path (bit 5 of
            // flagsAndPad[0]) the forward pass writes linear HDR into an offscreen
            // target and the grab copies THAT, so decoding it a second time would
            // darken every reflection. Metal gates the same decode the same way.
            // Roughness cone (twin of the Metal block): the GGX lobe spreads the rays
            // into a cone of half-angle ~ roughness^2; its footprint at the hit,
            // tan(cone) * hit distance, converted to grab pixels by the focal length
            // (the view-projection's clip-y row length times half the grab height)
            // over the hit's depth, picks the mip. Explicit LOD: implicit derivatives
            // are undefined behind the loop above.
            float tanCone = roughness * roughness;
            float p11 = length(vec3(lighting.viewProjection[0][1], lighting.viewProjection[1][1], lighting.viewProjection[2][1]));
            float focalPx = 0.5 * float(textureSize(ssrSceneColor, 0).y) * p11;
            float footprintPx = tanCone * ssrHitT * focalPx / max(ssrHitW, 1e-3);
            float maxLod = max(float(textureQueryLevels(ssrSceneColor)) - 1.0, 0.0);
            float ssrLod = clamp(log2(max(footprintPx, 1.0)), 0.0, maxLod);
            vec3 ssrColor = textureLod(ssrSceneColor, ssrHitUv, ssrLod).rgb;
            if ((lighting.flagsAndPad[0] & (1u << 5)) == 0u) {
                ssrColor = srgbToLinear(ssrColor);
            }
            // Fade at screen edges (reflections pop as rays exit the frame) and
            // on rough surfaces (this port marches sharp — no roughness cone).
            vec2 eLo = smoothstep(vec2(0.0), vec2(0.12), ssrHitUv);
            vec2 eHi = 1.0 - smoothstep(vec2(0.88), vec2(1.0), ssrHitUv);
            float edgeFade = eLo.x * eLo.y * eHi.x * eHi.y;
            float gloss = 1.0 - roughness;
            // Very rough surfaces fade where the mip chain can no longer stand in for the lobe.
            float roughFade = 1.0 - smoothstep(0.7, 1.0, roughness);
            vec3 ssrFres = ssrFresnel(NdotV, gloss, F0);
            vec3 replaced = mix(indirectSpecular, ssrColor * ssrFres * specularOn,
                edgeFade * roughFade);
            color += replaced - indirectSpecular;
            indirectSpecular = replaced;
        }
    }
    // Occlusion, applied once every specular term is final (probe and SSR
    // replace indirectSpecular above). `color` already holds the direct and
    // indirect terms, so an occluded share is taken back out as `x * (f - 1)`,
    // which is exactly `x *= f` on the total. Parity with
    // forward-fragment-ambient.metal and upstream's occludeDiffuse /
    // occludeSpecular (aoSpecOcc.js).
    if ((material.flags & FLAG_OCCLUDE_DIRECT) != 0u) {
        // occludeDirect: the direct diffuse, and the bake if there is one — upstream's
        // second occludeDiffuse runs after addLightMap and the light loop.
        vec3 occludedDiffuse = directDiffuse;
        if (vtFeatureEnabled(VT_FEATURE_LIGHTMAP_BIT)) {
            occludedDiffuse += indirectDiffuse;
        }
        color += occludedDiffuse * (ao - 1.0);
    }
    if (material.occludeSpecularMode != SPECOCC_NONE) {
        float specOcc = 1.0;
        if (material.occludeSpecularMode == SPECOCC_AO) {
            specOcc = ao;
        } else if (material.occludeSpecularMode == SPECOCC_GLOSSDEPENDENT) {
            // Approximated specular occlusion from AO (tri-Ace, CEDEC 2011).
            float specPow = exp2((1.0 - roughness) * 11.0);
            specOcc = clamp(pow(NdotV + ao, 0.01 * specPow) - 1.0 + ao, 0.0, 1.0);
        }
        specOcc = mix(1.0, specOcc, clamp(material.occludeSpecularIntensity, 0.0, 1.0));
        color += (directSpecular + indirectSpecular) * (specOcc - 1.0);
        directSpecular *= specOcc;
        indirectSpecular *= specOcc;
        if (vtFeatureEnabled(VT_FEATURE_SHEEN_BIT)) {
            // Sheen is not in `color` yet — the tail adds it after scaling the base
            // layer — so it is occluded in place rather than corrected out.
            sheenSpecularDirect *= specOcc;
            sheenSpecularIndirect *= specOcc;
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
                // Same NDC-to-texture mapping as the SSR march above.
                vec2 grabUv = clamp(projected.xy * invW * vec2(0.5, -0.5) + 0.5,
                    0.001, 0.999);

                float iorCh = 1.0 / etaCh;
                float iorToRoughness = clamp(1.0 - gloss, 0.0, 1.0) *
                    clamp(iorCh * 2.0 - 2.0, 0.0, 1.0);
                float refractionLod = grabMips * iorToRoughness;
                // Decoded only when the grab came from the gamma-encoded back
                // buffer; the camera-frame grab is already linear (see the SSR
                // block above).
                vec3 sampleColor =
                    textureLod(ssrSceneColor, grabUv, refractionLod).rgb;
                if ((lighting.flagsAndPad[0] & (1u << 5)) == 0u) {
                    sampleColor = srgbToLinear(sampleColor);
                }
                if (refrSamples == 1) {
                    refrColor = sampleColor;
                } else {
                    refrColor[ch] = sampleColor[ch];
                }
            }

            // Volume transmittance (KHR_materials_volume Beer's law); distance 0
            // transmits everything. Then the diffuse albedo, ONCE, as upstream's
            // combineColor applies it to the refraction mixed into dDiffuseLight - not
            // baseColor^(thickness + 1), which darkened a tinted volume several times
            // over (see the Metal tail).
            if (material.attenuationParams.w > 0.0) {
                vec3 attColor = clamp(material.attenuationParams.rgb, 0.0001, 1.0);
                refrColor *= exp(-(-log(attColor) / material.attenuationParams.w) *
                    thickness);
            }
            refrColor *= diffuseAlbedo;

            // Fresnel: grazing angles reflect more, normal incidence transmits.
            float F0ior = pow((1.0 - ior) / (1.0 + ior), 2.0);
            float fresnel = F0ior + (1.0 - F0ior) * pow(1.0 - NdotV, 5.0);
            float transmission = material.transmissionFactor * (1.0 - fresnel);

            // Replace surface diffuse with the refracted scene, keep specular.
            // Emissive is added after this block, so it survives on its own.
            vec3 specPart = directSpecular + indirectSpecular;
            color = mix(color, refrColor + specPart, clamp(transmission, 0.0, 1.0));
        } else if (vtFeatureEnabled(VT_FEATURE_ENV_ATLAS_BIT) &&
            lighting.envParams.y > 0.5 && (material.flags & (1u << 18)) == 0u &&
            material.transmissionFactor > 0.0) {
            // Env-atlas refraction (upstream refractionCube.js): the reflection lookup
            // along the REFRACTED direction. What stood here mixed toward this
            // fragment's own ambient (indirectDiffuse + indirectSpecular), which is
            // not a refraction at all: the surface came out opaque at any
            // transmission. Twin of the Metal tail's cube path.
            vec3 refrDir = refract(-V, N, 1.0 / max(material.refractionIndex, 1.001));
            vec3 refrColor = (dot(refrDir, refrDir) > 0.0)
                ? sampleEnvAtlas(normalize(refrDir), roughness) * max(lighting.envParams.x, 0.0)
                : vec3(0.0);
            if (material.attenuationParams.w > 0.0) {
                // Same Beer's law as the dynamic path: a^(t/d) == exp(-(-ln a/d)*t).
                refrColor *= pow(max(material.attenuationParams.rgb, vec3(1e-4)),
                    vec3(max(material.thickness, 0.0) / material.attenuationParams.w));
            }
            // The albedo TWICE, as upstream: refractionCube mixes refraction * albedo
            // into dDiffuseLight and combineColor multiplies that by the albedo again.
            // (The dynamic path applies it once, and so does upstream's.) No Fresnel
            // weight either - upstream's cube path has none.
            refrColor *= diffuseAlbedo * diffuseAlbedo;
            vec3 specPart = directSpecular + indirectSpecular;
            color = mix(color, refrColor + specPart,
                clamp(material.transmissionFactor, 0.0, 1.0));
        }
    }

