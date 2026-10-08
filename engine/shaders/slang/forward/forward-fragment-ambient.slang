// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
    // Shadow catcher REPLACES the shaded result with the accumulated shadow
    // factor as grayscale, so a ground plane can receive shadows from virtual
    // geometry over a real backdrop (AR compositing). Returns raw — deliberately
    // no fog, exposure, tonemap or gamma — because the value is a blend
    // coefficient, not a colour: with multiplicative blending (src * dst), white
    // leaves the framebuffer untouched and darker values darken it where shadows
    // fall. Encoding it would bend the shadow response.
    //
    // It returns here: after the light loop, before any indirect composition.
    if (vtFeatureEnabled(VT_FEATURE_SHADOW_CATCHER_BIT)) {
        outColor = float4(float3(dShadowCatcher), 1.0);
        return outColor;
    }

    // Indirect lighting, split in two: the DIFFUSE irradiance comes from the first
    // of SH light probes, the environment atlas' ambient rect, or the flat ambient;
    // the SPECULAR reflection comes from the environment atlas whenever it is
    // bound, whatever supplied the diffuse. Do not put the probes and the atlas in ONE if/else-if:
    // a scene carrying both would lose every environment reflection the moment its
    // probes were enabled. Diffuse and specular are kept apart so ambient
    // occlusion can treat them separately (see the occlusion block below).
    float3 ambientIrradiance;
    if (vtFeatureEnabled(VT_FEATURE_LIGHT_PROBES_BIT)) {
        // 9-coefficient irradiance in the world normal direction (coefficients
        // premultiplied).
        float3 shN = cubeMapRotate(N);
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
            float3(0.0));
    } else {
        ambientIrradiance = lighting.ambient.rgb;
    }
    // Mirrors the specular part of the indirect contribution exactly as it lands
    // in `color` below, AO factor included. SSR replaces that term where the
    // reflection ray hits on-screen geometry, so it only has to add the
    // difference rather than restructure the accumulation.
    float3 indirectSpecular = float3(0.0);
    if (vtFeatureEnabled(VT_FEATURE_ENV_ATLAS_BIT) &&
        lighting.envParams.y > 0.5 &&
        // bit 18: useSkybox off, so this material keeps
        // the probe or flat ambient above and takes no scene reflection.
        (material.flags & (1u << 18)) == 0u) {
        float intensity = max(lighting.envParams.x, 0.0);

        if (!vtFeatureEnabled(VT_FEATURE_LIGHT_PROBES_BIT)) {
            // Diffuse irradiance from the atlas' Lambert rect (the negate-X matches
            // the engine's atlas lookup handedness). Probes take priority over it.
            float3 envN = cubeMapRotate(N);
            float3 diffDir = float3(-envN.x, envN.y, envN.z);
            ambientIrradiance = decodeEnv(envAtlas.Sample(mapAmbientUv(dirToEquirect(diffDir)))) * intensity;
        }

        // Specular: reflect, pick a mip, trilinear between levels, with the shiny
        // path for a mirror. Anisotropic materials bend the normal first
        // (common-brdf.slang).
        float3 R = vtFeatureEnabled(VT_FEATURE_ANISOTROPY_BIT)
            ? getReflDirAniso(N, V, anisoB, 1.0 - roughness, anisoIntensity)
            : reflect(-V, N);
        float3 envR = cubeMapRotate(R);
        float3 specDir = float3(-envR.x, envR.y, envR.z);
        float2 envUv = dirToEquirect(specDir);
        float level = clamp(roughness * 5.0, 0.0, 5.0);
        float l0 = floor(level);

        // Screen-space mip for the sharp rect. The second
        // derivative pair is taken on frac(u + 0.5) so the azimuthal wrap, where u
        // jumps 1 -> 0 across one pixel, does not read as an enormous gradient and
        // force the blurriest mip along that seam.
        float2 shinyUvFull = envUv * ATLAS_SIZE;
        float2 dxA = ddx(shinyUvFull);
        float2 dyA = ddy(shinyUvFull);
        float2 uvWrapped = float2(frac(envUv.x + 0.5), envUv.y) * ATLAS_SIZE;
        float2 dxB = ddx(uvWrapped);
        float2 dyB = ddy(uvWrapped);
        float maxd = min(max(dot(dxA, dxA), dot(dyA, dyA)),
                         max(dot(dxB, dxB), dot(dyB, dyB)));
        float shinyLevel = clamp(0.5 * log2(max(maxd, 1e-12)) - 1.0, 0.0, 5.0);
        float shinyL0 = floor(shinyLevel);

        // A mirror (level 0) takes the unconvolved shiny rect; anything rougher
        // takes the prefiltered chain, and both blend toward the next roughness mip.
        float3 envA;
        if (l0 == 0.0) {
            float3 shinyA = decodeEnv(envAtlas.Sample(mapShinyUv(envUv, shinyL0)));
            float3 shinyB = decodeEnv(envAtlas.Sample(mapShinyUv(envUv, shinyL0 + 1.0)));
            envA = lerp(shinyA, shinyB, shinyLevel - shinyL0);
        } else {
            envA = decodeEnv(envAtlas.Sample(mapRoughnessUv(envUv, l0)));
        }
        float3 envB = decodeEnv(envAtlas.Sample(mapRoughnessUv(envUv, l0 + 1.0)));
        float3 prefiltered = lerp(envA, envB, level - l0) * intensity;

        // Gloss-aware Fresnel for the environment term (getFresnel), the same
        // helper this file uses for SSR. The hand-rolled Schlick-roughness variant
        // here returned up to (1 - roughness) at grazing angles where this returns
        // ~F0, so the environment specular ran far too hot.
        float3 Fr = ssrFresnel(NdotV, 1.0 - roughness, F0);
        if (vtFeatureEnabled(VT_FEATURE_IRIDESCENCE_BIT)) {
            Fr = lerp(Fr, iridFresnel, iridIntensity);
        }
        indirectSpecular = prefiltered * Fr * specularOn;

        if (vtFeatureEnabled(VT_FEATURE_CLEARCOAT_BIT)) {
            // Clearcoat reflection of the environment: the atlas along the coat's
            // reflection at the coat's roughness — the shiny rect's first two levels
            // when the coat is a mirror, the prefiltered chain otherwise — with the
            // coat's fixed-F0 Fresnel. Composed in the tail, not here: the base's
            // specular stays in indirectSpecular alone, which occlusion and SSR read.
            float3 ccR = cubeMapRotate(reflect(-V, ccNormalW));
            float2 ccEnvUv = dirToEquirect(normalize(float3(-ccR.x, ccR.y, ccR.z)));
            float ccLevel = clamp(1.0 - ccGlossiness, 0.0, 1.0) * 5.0;
            float ccL0 = floor(ccLevel);
            float3 ccEnv;
            if (ccL0 == 0.0) {
                float3 a = decodeEnv(envAtlas.Sample(mapShinyUv(ccEnvUv, 0.0)));
                float3 b = decodeEnv(envAtlas.Sample(mapShinyUv(ccEnvUv, 1.0)));
                ccEnv = lerp(a, b, ccLevel);
            } else {
                float3 a = decodeEnv(envAtlas.Sample(mapRoughnessUv(ccEnvUv, ccL0)));
                float3 b = decodeEnv(envAtlas.Sample(mapRoughnessUv(ccEnvUv, ccL0 + 1.0)));
                ccEnv = lerp(a, b, ccLevel - ccL0);
            }
            ccReflection = ccEnv * intensity * getFresnelCC(max(dot(ccNormalW, V), 0.0));
        }
    }
    // No kD on the irradiance: scaling it by (1 - Fr) * (1 - metallic) would apply
    // (1 - metallic) a SECOND time, because diffuseAlbedo already carries it. No
    // specular floor without an atlas either: `ambient * F0` would light metals
    // from nothing in scenes with no environment.
    // Right after the ambient is added, the ambient diffuse is
    // scaled by (1 - specularity) per channel when the material renders specular.
    ambientIrradiance *= lerp(float3(1.0), float3(1.0) - F0, specularOn);
    // The material's ambient tint: the
    // bake accumulator above it keeps the untinted ambient, and a lightmap below
    // replaces the tinted term.
    float3 indirectDiffuse = ambientIrradiance * material.ambientTint.rgb * diffuseAlbedo;
    bakeDiffuseLight += ambientIrradiance;
    // Sheen image-based lighting: sample the atlas along the reflection at the
    // sheen roughness, scaled by the analytical directional albedo instead of the
    // DFG lookup. Without it a sheened surface lit only by an environment shows
    // no sheen.
    if (vtFeatureEnabled(VT_FEATURE_SHEEN_BIT) &&
        vtFeatureEnabled(VT_FEATURE_ENV_ATLAS_BIT) && lighting.envParams.y > 0.5) {
        float3 sheenR = cubeMapRotate(reflect(-V, N));
        float2 sheenEnvUv = dirToEquirect(normalize(float3(-sheenR.x, sheenR.y, sheenR.z)));
        float sheenLevel = clamp(sheenRoughness, 0.0, 1.0) * 5.0;
        float sheenL0 = floor(sheenLevel);

        float3 sheenEnvColor;
        if (sheenL0 == 0.0) {
            float3 sa = decodeEnv(envAtlas.Sample(mapShinyUv(sheenEnvUv, 0.0)));
            float3 sb = decodeEnv(envAtlas.Sample(mapShinyUv(sheenEnvUv, 1.0)));
            sheenEnvColor = lerp(sa, sb, sheenLevel);
        } else {
            float3 sa = decodeEnv(envAtlas.Sample(mapRoughnessUv(sheenEnvUv, sheenL0)));
            float3 sb = decodeEnv(envAtlas.Sample(mapRoughnessUv(sheenEnvUv, sheenL0 + 1.0)));
            sheenEnvColor = lerp(sa, sb, sheenLevel - sheenL0);
        }
        sheenSpecularIndirect = sheenEnvColor * max(lighting.envParams.x, 0.0)
            * sheenTint * sheenIBLApprox(max(dot(N, V), 0.001), sheenRoughness);
    }

    // Ambient occlusion on the AMBIENT diffuse: it is occluded before the
    // lightmap and before the light loop, so the
    // bake and the direct light are occluded only under occludeDirect — handled
    // in the occlusion block after the specular terms are final.
    indirectDiffuse *= ao;
    if (vtFeatureEnabled(VT_FEATURE_LIGHTMAP_BIT)) {
        // The bake REPLACES the ambient diffuse rather than adding to it, so that a lightmapped surface is not lit twice by what the bake already
        // contains.
        // Lightmaps store LINEAR light (see the bake output and Lightmapper's encoder).
        indirectDiffuse = max(lightMap.Sample(fragUV1).rgb, float3(0.0)) * diffuseAlbedo;
    }
    color += indirectDiffuse + indirectSpecular;
    if (vtFeatureEnabled(VT_FEATURE_REFLECTION_PROBE_BIT)) {
        // Every reflection source is sampled along the one (bent) reflection direction.
        float3 reflectDir = vtFeatureEnabled(VT_FEATURE_ANISOTROPY_BIT)
            ? getReflDirAniso(N, V, anisoB, 1.0 - roughness, anisoIntensity)
            : reflect(-V, N);
        float3 sampleDir = reflectDir;

        // Box projection: intersect the reflection
        // ray with the probe box, then re-aim from the box CENTRE — that is what
        // makes a flat cubemap track a room's walls as the surface moves. Aiming
        // from the probe's own position instead, and leaving the result
        // unnormalised, pointed the lookup somewhere else entirely.
        if (lighting.reflectionProbeParams.x > 0.5) {
            float3 boxMin = lighting.reflectionProbeBoxMin.xyz;
            float3 boxMax = lighting.reflectionProbeBoxMax.xyz;
            float3 invDir = 1.0 / reflectDir;
            float3 rbmax = (boxMax - fragWorldPos) * invDir;
            float3 rbmin = (boxMin - fragWorldPos) * invDir;
            float3 rbminmax = select(reflectDir > float3(0.0), rbmax, rbmin);
            float fa = min(min(rbminmax.x, rbminmax.y), rbminmax.z);
            float3 posOnBox = fragWorldPos + reflectDir * fa;
            sampleDir = normalize(posOnBox - (boxMin + boxMax) * 0.5);
        }

        // The engine's cube convention flips X, the same as every sky path here.
        float3 cubeDir = float3(-sampleDir.x, sampleDir.y, sampleDir.z);
        float gloss = 1.0 - roughness;
        float probeLod = clamp(1.0 - gloss, 0.0, 1.0) * lighting.reflectionProbeParams.z;
        // The captured cube is written gamma-encoded, so it owes a decode before it
        // can be added to linear light — the same split the base-colour and
        // emissive maps follow. Without it the probe reads far too bright, which is
        // what washed every metallic surface here out to pale pastel.
        float3 probeSpecular =
            pow(max(reflectionProbeCube.SampleLevel(linearClampSampler, cubeDir, probeLod).rgb, float3(0.0)),
                float3(2.2)) * lighting.reflectionProbeParams.y;

        // Gloss-aware Fresnel, not a raw F0 multiply.
        float3 probeFresnel = ssrFresnel(NdotV, gloss, F0);
        if (vtFeatureEnabled(VT_FEATURE_IRIDESCENCE_BIT)) {
            probeFresnel = lerp(probeFresnel, iridFresnel, iridIntensity);
        }

        // The probe REPLACES the environment specular. `color` already carries that
        // term from the block above, so add only the difference — adding the probe
        // outright counted both.
        float3 replaced = probeSpecular * probeFresnel * specularOn;
        color += replaced - indirectSpecular;
        indirectSpecular = replaced;
    }
    // Screen-space reflections: march the reflection ray against the scene depth
    // grab and sample the scene colour grab at the hit, blending OVER the
    // probe/env-atlas specular where the ray lands on on-screen geometry. The colour
    // is read at an explicit LOD, the depth is point-sampled (nearestClampSampler,
    // see the head) because it is a silhouette test, and each crossing is bisected.
    if (vtFeatureEnabled(VT_FEATURE_SSR_BIT) &&
        lighting.cameraNearFar.z > 0.5 && lighting.cameraNearFar.w > 0.5) {
        float ssrNear = lighting.cameraNearFar.x;
        float ssrFar = lighting.cameraNearFar.y;
        float3 ssrR = reflect(-V, N);

        const int SSR_STEPS = 48;
        const int SSR_REFINE = 6;
        float ssrMaxDist = 0.4 * (ssrFar - ssrNear);
        float ssrStep = ssrMaxDist / float(SSR_STEPS);
        float ssrThickness = ssrStep * 1.25;   // view-space tolerance behind the surface

        float2 ssrHitUv = float2(0.0);
        float ssrHit = 0.0;
        float ssrHitT = 0.0;   // distance along the ray to the hit
        float ssrHitW = 1.0;   // the hit's view depth
        float tPrev = 0.0;
        for (int i = 1; i <= SSR_STEPS; ++i) {
            float t = ssrStep * float(i);
            float3 samplePos = fragWorldPos + ssrR * t;
            float4 clip = mul(float4(samplePos, 1.0), lighting.viewProjection);
            if (clip.w <= 0.0) break;                     // behind the camera
            // The Vulkan backend rasterises through a NEGATED-height viewport so that
            // projection matrices written for Metal work unchanged, which puts
            // NDC +Y at the TOP row of every target, back buffer and offscreen
            // alike. A projected point therefore maps to a texture coordinate
            // exactly as it does on Metal, Y included.
            float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
            if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break;
            float rawDepth = sceneDepthGrab.SampleLevel(nearestClampSampler, uv, 0.0).r;
            float sceneZ = (ssrNear * ssrFar) / max(ssrFar - rawDepth * (ssrFar - ssrNear), 1e-6);
            float diff = clip.w - sceneZ;                 // >0 = behind the surface
            if (diff > 0.0) {
                // Any crossing is a candidate; bisect to it and judge the thickness there,
                // since a hit accepted at the coarse sample sits up to a step past the
                // intersection. A rejected silhouette jump does not end the march.
                float lo = tPrev, hi = t;
                float2 hitUv = uv;
                float hitDiff = diff;
                float hitT = t, hitW = clip.w;
                for (int k = 0; k < SSR_REFINE; ++k) {
                    float mid = 0.5 * (lo + hi);
                    float4 c = mul(float4(fragWorldPos + ssrR * mid, 1.0), lighting.viewProjection);
                    float2 u = c.xy / c.w * float2(0.5, -0.5) + 0.5;
                    float rd = sceneDepthGrab.SampleLevel(nearestClampSampler, u, 0.0).r;
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
            // darken every reflection.
            // Roughness cone: the GGX lobe spreads the rays into a cone of half-angle
            // ~ roughness^2; its footprint at the hit, tan(cone) * hit distance,
            // converted to grab pixels by the focal length (the view-projection's
            // clip-y row length times half the grab height) over the hit's depth,
            // picks the mip. Explicit LOD: implicit derivatives
            // are undefined behind the loop above.
            float tanCone = roughness * roughness;
            float p11 = length(float3(lighting.viewProjection[0][1], lighting.viewProjection[1][1], lighting.viewProjection[2][1]));
            float focalPx = 0.5 * float(vtTextureSize(sceneColorGrab, 0).y) * p11;
            float footprintPx = tanCone * ssrHitT * focalPx / max(ssrHitW, 1e-3);
            float maxLod = max(float(vtTextureLevels(sceneColorGrab)) - 1.0, 0.0);
            float ssrLod = clamp(log2(max(footprintPx, 1.0)), 0.0, maxLod);
            float3 ssrColor = sceneColorGrab.SampleLevel(linearClampSampler, ssrHitUv, ssrLod).rgb;
            if ((lighting.flagsAndPad[0] & (1u << 5)) == 0u) {
                ssrColor = srgbToLinear(ssrColor);
            }
            // Fade at screen edges (reflections pop as rays exit the frame).
            float2 eLo = smoothstep(float2(0.0), float2(0.12), ssrHitUv);
            float2 eHi = 1.0 - smoothstep(float2(0.88), float2(1.0), ssrHitUv);
            float edgeFade = eLo.x * eLo.y * eHi.x * eHi.y;
            float gloss = 1.0 - roughness;
            // Very rough surfaces fade where the mip chain can no longer stand in for the lobe.
            float roughFade = 1.0 - smoothstep(0.7, 1.0, roughness);
            float3 ssrFres = ssrFresnel(NdotV, gloss, F0);
            float3 replaced = lerp(indirectSpecular, ssrColor * ssrFres * specularOn,
                edgeFade * roughFade);
            color += replaced - indirectSpecular;
            indirectSpecular = replaced;
        }
    }
    // Occlusion, applied once every specular term is final (probe and SSR
    // replace indirectSpecular above). `color` already holds the direct and
    // indirect terms, so an occluded share is taken back out as `x * (f - 1)`,
    // which is exactly `x *= f` on the total.
    if ((material.flags & FLAG_OCCLUDE_DIRECT) != 0u) {
        // occludeDirect: the direct diffuse, and the bake if there is one — this
        // second occlusion runs after the lightmap and the light loop.
        float3 occludedDiffuse = directDiffuse;
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
        specOcc = lerp(1.0, specOcc, clamp(material.occludeSpecularIntensity, 0.0, 1.0));
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
        // The scalar maps modulate their factors per pixel, sampled once for both refraction paths.
        float refractionFactor = material.transmissionFactor;
        float refractionThickness = material.thickness;
        if (material.mapChannelParams.w >= 0.0) {
            refractionFactor *= refractionMap.Sample(materialSampler, fragUV0)[int(material.mapChannelParams.w)];
        }
        if (material.mapChannelParams.z >= 0.0) {
            refractionThickness *= thicknessMap.Sample(materialSampler, fragUV0)[int(material.mapChannelParams.z)];
        }
        if (vtFeatureEnabled(VT_FEATURE_DYNAMIC_REFRACTION_BIT) &&
            lighting.cameraNearFar.z > 0.5 && refractionFactor > 0.0) {
            // Dynamic grab-pass refraction:
            // sample the mid-frame scene colour grab at the screen position of
            // the refracted exit point instead of the environment atlas.
            float ior = max(material.refractionIndex, 1.001);
            float thickness = max(refractionThickness, 0.0);

            // Dispersion (KHR_materials_dispersion): spread the refraction eta
            // per channel and sample R/G/B separately.
            float dispersion = max(material.dispersionParams.x, 0.0);
            float eta = 1.0 / ior;
            float halfSpread = (ior - 1.0) * 0.025 * dispersion;
            int refrSamples = (dispersion > 0.0) ? 3 : 1;

            // Mip range of the grab chain; higher IOR and rougher surfaces read
            // blurrier scene colour.
            float grabMips =
                log2(max(float(vtTextureSize(sceneColorGrab, 0).x), 2.0));
            float gloss = 1.0 - roughness;

            float3 refrColor = float3(0.0);
            // The distance the volume absorbs over: the length of the LAST refraction
            // vector built (the blue channel's under dispersion), thickness times the
            // model's scale along the refracted direction.
            float refractionLength = 0.0;
            for (int ch = 0; ch < refrSamples; ++ch) {
                float etaCh = (refrSamples == 1)
                    ? eta : (eta + halfSpread * float(ch - 1));
                float3 refrDir = refract(-V, N, etaCh);

                // Refraction vector scaled by volume thickness and the model's
                // per-axis scale; total internal
                // reflection falls back to the unshifted surface point.
                float3 modelScale = input.modelScale;
                float3 refractionVector = (dot(refrDir, refrDir) > 0.0)
                    ? normalize(refrDir) * thickness * modelScale : float3(0.0);
                refractionLength = length(refractionVector);

                float4 projected = mul(float4(fragWorldPos + refractionVector, 1.0), lighting.viewProjection);
                float invW = 1.0 / max(projected.w, 1e-6);
                // Same NDC-to-texture mapping as the SSR march above.
                float2 grabUv = clamp(projected.xy * invW * float2(0.5, -0.5) + 0.5,
                    0.001, 0.999);

                float iorCh = 1.0 / etaCh;
                float iorToRoughness = clamp(1.0 - gloss, 0.0, 1.0) *
                    clamp(iorCh * 2.0 - 2.0, 0.0, 1.0);
                float refractionLod = grabMips * iorToRoughness;
                // Decoded only when the grab came from the gamma-encoded back
                // buffer; the camera-frame grab is already linear (see the SSR
                // block above).
                float3 sampleColor =
                    sceneColorGrab.SampleLevel(linearClampSampler, grabUv, refractionLod).rgb;
                if ((lighting.flagsAndPad[0] & (1u << 5)) == 0u) {
                    sampleColor = srgbToLinear(sampleColor);
                }
                if (refrSamples == 1) {
                    refrColor = sampleColor;
                } else {
                    refrColor[ch] = sampleColor[ch];
                }
            }

            // Volume transmittance (KHR_materials_volume Beer's law) over the
            // refracted path length, which carries the model's scale; distance 0
            // transmits everything. Then the diffuse albedo, ONCE, applied to the
            // refraction mixed into the diffuse light - not
            // baseColor^(thickness + 1), which darkened a tinted volume several times
            // over.
            if (material.attenuationParams.w > 0.0) {
                float3 attColor = clamp(material.attenuationParams.rgb, 0.0001, 1.0);
                refrColor *= exp(-(-log(attColor) / material.attenuationParams.w) *
                    refractionLength);
            }

            // Fresnel: the light the surface reflects does not enter it. The same
            // gloss-aware Fresnel the reflections are weighted by (iridescence
            // included), so at a glossy rim the refraction fades out and the
            // reflection takes over.
            float3 refrFresnel = getFresnel(dot(N, V), gloss, F0);
            if (vtFeatureEnabled(VT_FEATURE_IRIDESCENCE_BIT)) {
                refrFresnel = lerp(refrFresnel, iridFresnel, iridIntensity);
            }
            refrColor *= diffuseAlbedo * (float3(1.0) - refrFresnel);

            // Replace surface diffuse with the refracted scene by the transmission
            // factor, keep specular. Emissive is added in the tail, after this block.
            float3 specPart = directSpecular + indirectSpecular;
            color = lerp(color, refrColor + specPart, clamp(refractionFactor, 0.0, 1.0));
        } else if (vtFeatureEnabled(VT_FEATURE_ENV_ATLAS_BIT) &&
            lighting.envParams.y > 0.5 && (material.flags & (1u << 18)) == 0u &&
            refractionFactor > 0.0) {
            // Env-atlas refraction: the reflection lookup
            // along the REFRACTED direction — not a mix toward this fragment's own
            // ambient (indirectDiffuse + indirectSpecular), which is not a refraction
            // at all and reads opaque at any transmission.
            float3 refrDir = refract(-V, N, 1.0 / max(material.refractionIndex, 1.001));
            float3 refrColor = (dot(refrDir, refrDir) > 0.0)
                ? sampleEnvAtlas(normalize(refrDir), roughness) * max(lighting.envParams.x, 0.0)
                : float3(0.0);
            if (material.attenuationParams.w > 0.0) {
                // Same Beer's law as the dynamic path: a^(t/d) == exp(-(-ln a/d)*t).
                refrColor *= pow(max(material.attenuationParams.rgb, float3(1e-4)),
                    float3(max(refractionThickness, 0.0) / material.attenuationParams.w));
            }
            // The albedo TWICE, reproducing upstream: its cube path mixes refraction *
            // albedo into the diffuse light and multiplies that by the albedo again.
            // (The dynamic path applies it once.) No Fresnel weight either - the
            // cube path has none.
            refrColor *= diffuseAlbedo * diffuseAlbedo;
            float3 specPart = directSpecular + indirectSpecular;
            color = lerp(color, refrColor + specPart,
                clamp(refractionFactor, 0.0, 1.0));
        }
    }

