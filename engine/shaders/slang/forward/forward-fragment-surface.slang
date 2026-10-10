// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
[shader("fragment")]
float4 forwardFragment(ForwardVarying input, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    // The inputs under the names every chunk reads them by.
    float4 outColor = float4(0.0, 0.0, 0.0, 0.0);
    vtFragCoord = input.position;
    vtFrontFacing = isFrontFace;
    float3 fragWorldPos = input.worldPos;
    float3 fragWorldNormal = input.worldNormal;
    float2 fragUV0 = input.uv0;
    float2 fragUV1 = input.uv1;
    float4 fragWorldTangent = input.worldTangent;
    float fragViewDepth = input.viewDepth;
    float4 fragColor = input.color;
    // Skybox: sample the environment along the view direction and output it
    // directly — no surface lighting. The sky vertex stage carries the pre-transform
    // vertex position in the normal varying, which IS the view ray: the world position
    // minus the camera would cancel catastrophically at globe scale (both ~1e7 m).
    if (vtFeatureEnabled(VT_FEATURE_SKYBOX_BIT)) {
        // Dome projection: view direction from the dome center rather than
        // the camera so the flattened bottom hemisphere reads as a ground
        // plane (tripod projection).
        uint skyFlags = uint(lighting.skyParams2.w + 0.5);
        float3 dir = ((skyFlags & 2u) != 0u)
            ? normalize(fragWorldPos - lighting.skyParams2.xyz)
            : normalize(fragWorldNormal);
        float3 sky;
        if (vtFeatureEnabled(VT_FEATURE_ATMOSPHERE_BIT)) {
            // Nishita scattering replaces the cubemap/atlas sky entirely, and
            // takes precedence over both.
            sky = nishitaScatter(dir);
        } else if ((skyFlags & 1u) != 0u) {
            // High-res skybox cubemap (negated X matches the engine's atlas
            // lookup handedness).
            float intensity = max(lighting.envParams.x, 0.0);
            float3 cubeDir = cubeMapRotate(dir);
            sky = decodeEnv(skyboxCube.Sample(linearClampSampler, float3(-cubeDir.x, cubeDir.y, cubeDir.z))) * intensity;
        } else if (vtFeatureEnabled(VT_FEATURE_ENV_ATLAS_BIT) &&
                   lighting.envParams.y > 0.5) {
            // Negate X, the same way every other env-atlas lookup does (see
            // the diffuse and specular IBL directions in
            // forward-fragment-ambient) and the cubemap branch just above.
            // Without it the sky renders mirrored: an exact left-right
            // flip, which a full-width mean cannot see because mirroring
            // preserves it.
            float intensity = max(lighting.envParams.x, 0.0);
            float3 rotatedDir = cubeMapRotate(dir);
            float3 atlasDir = float3(-rotatedDir.x, rotatedDir.y, rotatedDir.z);
            sky = decodeEnv(envAtlas.Sample(mapRoughnessUv(dirToEquirect(atlasDir),
                                    max(lighting.envParams.w, 0.0)))) * intensity;
        } else {
            sky = material.baseColor.rgb;
        }
        // Under the camera-frame path (bit 5 of flagsAndPad[0]) the sky, like
        // every other forward draw, must leave linear HDR for compose to expose,
        // tonemap and gamma-encode. Without this the sky would be written already
        // gamma-encoded into the linear HDR target and compose would encode it a
        // second time.
        if ((lighting.flagsAndPad[0] & (1u << 5)) != 0u) {
            outColor = float4(max(sky, float3(0.0)), 1.0);
            return outColor;
        }
        sky = applyToneMap(sky);                         // exposure + tonemap
        outColor = float4(pow(max(sky, float3(0.0)), float3(1.0 / 2.2)), 1.0); // display-gamma encode
        return outColor;
    }

    // Per-map UVs: select the UV set by flag bit, then apply that map's own
    // transform, not the base-color transform for every map.
    float2 uvBase = applyUvTransform(
        ((material.flags & FLAG_BASE_UV1) != 0u) ? fragUV1 : fragUV0,
        material.baseColorTransform0, material.baseColorTransform1);
    float2 uvNormal = applyUvTransform(
        ((material.flags & FLAG_NORMAL_UV1) != 0u) ? fragUV1 : fragUV0,
        material.normalTransform0, material.normalTransform1);
    float2 uvMetalRough = applyUvTransform(
        ((material.flags & FLAG_METALROUGH_UV1) != 0u) ? fragUV1 : fragUV0,
        material.metalRoughTransform0, material.metalRoughTransform1);
    float2 uvGloss = applyUvTransform(fragUV0, material.glossTransform0, material.glossTransform1);
    float2 uvOcclusion = applyUvTransform(
        ((material.flags & FLAG_OCCLUSION_UV1) != 0u) ? fragUV1 : fragUV0,
        material.occlusionTransform0, material.occlusionTransform1);
    float2 uvEmissive = applyUvTransform(
        ((material.flags & FLAG_EMISSIVE_UV1) != 0u) ? fragUV1 : fragUV0,
        material.emissiveTransform0, material.emissiveTransform1);

    // Parallax occlusion mapping offsets all material UVs before any sampling.
    // The flags bit matters as well as the feature gate: an unbound image reads
    // as white here, which would offset UVs on a material that has no height map.
    // Parallax self-shadowing state, consumed by the directional light in
    // forward-fragment-lights.
    float3 parallaxTangent = float3(1.0, 0.0, 0.0);
    float3 parallaxBitangent = float3(0.0, 0.0, 1.0);
    float3 parallaxNormalGeom = float3(0.0, 1.0, 0.0);
    float2 parallaxUv = float2(0.0);
    float parallaxSurfaceDepth = 0.0;
    bool parallaxShadowActive = false;

    if (vtFeatureEnabled(VT_FEATURE_PARALLAX_BIT) &&
        (material.flags & FLAG_HAS_HEIGHTMAP) != 0u &&
        material.heightMapFactor > 0.0) {
        float3 nGeom = normalize(fragWorldNormal);
        float3 vPar = normalize(lighting.cameraPosExposure.xyz - fragWorldPos);
        float3 tPar = fragWorldTangent.xyz;
        if (dot(tPar, tPar) >= 1e-6) {
            tPar = normalize(tPar);
            // The tangent frame is built from the unflipped normal; a back face of a
            // double-sided material flips only its normal column, so the view still
            // enters the height field from above.
            float3 bPar = normalize(cross(nGeom, tPar)) * fragWorldTangent.w;
            if (vtFeatureEnabled(VT_FEATURE_DOUBLE_SIDED_BIT) && !vtFrontFacing) {
                nGeom = -nGeom;
            }
            // dot(basis, V) transforms the world view vector into tangent space.
            float3 viewDirTS = normalize(
                float3(dot(tPar, vPar), dot(bPar, vPar), dot(nGeom, vPar)));
            float heightBase = clamp(material.heightMapParams.x, 0.0, 1.0);
            float2 pomUv = parallaxOcclusionMap(
                uvBase, viewDirTS, material.heightMapFactor, heightBase);
            float2 uvDelta = pomUv - uvBase;
            uvBase       += uvDelta;
            uvNormal     += uvDelta;
            uvMetalRough += uvDelta;
            uvGloss      += uvDelta;
            uvOcclusion  += uvDelta;
            uvEmissive   += uvDelta;

            parallaxTangent = tPar;
            parallaxBitangent = bPar;
            parallaxNormalGeom = nGeom;
            parallaxUv = pomUv;
            parallaxSurfaceDepth = parallaxSampleDepth(pomUv, heightBase);
            parallaxShadowActive = material.heightMapParams.y > 0.0;
        }
    }

    // Under MSDF the base slot holds a distance field, read by the unlit path below.
    float4 baseSample = (vtFeatureEnabled(VT_FEATURE_BASE_COLOR_MAP_BIT) && !vtFeatureEnabled(VT_FEATURE_MSDF_BIT))
        ? baseColorMap.Sample(uvBase) : float4(1.0);
    // fragColor is float4(1) except for the vertex-color / point-cloud vertex
    // variants, which feed the mesh's per-vertex color through.
    // Two lanes, diffuse and emissive vertex colour:
    // flag bit 28 takes the color OFF the diffuse lane (so a zero flags word keeps
    // it on the diffuse) and bit 23 routes it to emissive instead. Alpha always
    // modulates opacity.
    bool diffuseVertexColorOff = (material.flags & (1u << 28)) != 0u;
    float4 vertexTint = diffuseVertexColorOff ? float4(1.0, 1.0, 1.0, fragColor.a) : fragColor;
    // Material colours are authored in gamma space, so the
    // factor AND the texture sample are decoded to linear before lighting;
    // skipping either leaves every surface brighter and less saturated.
    // An instanced draw with per-instance colours (an 80-byte instance stride) takes its
    // base colour from the instance instead of the material.
    const float4 baseFactor = vtFeatureEnabled(VT_FEATURE_INSTANCING_COLOR_BIT) ? input.instanceColor : material.baseColor;
    float3 baseLinear = srgbToLinear(baseFactor.rgb) * srgbToLinear(baseSample.rgb);
    float4 albedo = float4(baseLinear * vertexTint.rgb,
        baseFactor.a * baseSample.a * vertexTint.a);

    // Opacity map, flags bit 19: one channel (opacityTransform0.w) on the base colour's UV
    // set under its own transform, as the shadow frontend reads it.
    if ((material.flags & (1u << 19)) != 0u) {
        float2 uvOpacity = ((material.flags & (1u << 4)) != 0u) ? fragUV1 : fragUV0;
        uvOpacity = applyUvTransform(uvOpacity, material.opacityTransform0, material.opacityTransform1);
        albedo.a *= opacityMap.Sample(materialSampler, uvOpacity)[int(material.opacityTransform0.w)];
    }

    // DEBUGPASS_LIGHTING: neutralize albedo
    // before it feeds diffuseAlbedo/F0 so the lit result shows the lighting
    // alone rather than the texture. Unlike every other debug mode this one does
    // NOT replace the output — it falls through to the normal lit path, and so
    // still receives fog and tonemapping.
    if (vtFeatureEnabled(VT_FEATURE_DEBUG_PASS_BIT) &&
        lighting.flagsAndPad[1] == VT_DEBUGPASS_LIGHTING) {
        albedo.rgb = float3(0.5);
    }

    if (vtFeatureEnabled(VT_FEATURE_ALPHA_TEST_BIT) &&
        albedo.a < material.alphaCutoff) {
        discard;
    }

    // Opacity dithering: screen-space ordered
    // dither turns partial opacity into a discard pattern so transparency
    // renders in the opaque pass with correct depth. The matrix is chosen per
    // material via flags bits 25-27 (DitherMode) — a runtime value, not a shader
    // variant, so switching matrices needs no recompile. The pattern moves
    // per frame while the camera jitters for TAA (lighting.ditherJitter).
    if (vtFeatureEnabled(VT_FEATURE_OPACITY_DITHER_BIT)) {
        // The alpha dither decouples the two strengths: opacity keeps driving the
        // alpha blend while this value alone drives the dither density. Negative means
        // unset, which falls back to the coupled behaviour (opacity drives both).
        float ditherStrength = material.dispersionParams.y;
        bool hasAlphaDither = ditherStrength >= 0.0;
        float ditherAlpha = hasAlphaDither ? ditherStrength : albedo.a;

        if (ditherDiscards((material.flags >> 25) & 0x7u, vtFragCoord.xy, ditherAlpha,
                lighting.ditherJitter.xy)) {
            discard;
        }

        // Coupled (legacy) use is an opaque-pass technique, so forcing alpha keeps the
        // target's alpha channel clean. Decoupled use is the blend-AND-dither case,
        // where alpha must survive to drive the blend.
        if (!hasAlphaDither) {
            albedo.a = 1.0;
        }
    }

    // Picker id pass: after the alpha test and dither, write
    // the mesh instance's id, packed by the picker into the base colour's rgb.
    if (vtFeatureEnabled(VT_FEATURE_PICK_BIT)) {
        outColor = float4(material.baseColor.rgb, 1.0);
        return outColor;
    }

    // Point-cloud (unlit) path: the point vertex variant writes a zero world
    // normal as its sentinel — no surface lighting, just exposure + tonemap +
    // gamma on the tinted color.
    if (vtFeatureEnabled(VT_FEATURE_UNLIT_BIT) ||
        dot(fragWorldNormal, fragWorldNormal) < 1e-6) {
        // Emissive still contributes here: an unlit material drops the lights
        // but keeps the emissive lane (that is how a decal material draws at
        // all).
        float3 unlitEmissive = material.emissiveColor.rgb;
        if (vtFeatureEnabled(VT_FEATURE_EMISSIVE_MAP_BIT)) {
            // Decoded as the lit path decodes it; read as raw sRGB an unlit emissive
            // map would come out too bright.
            unlitEmissive *= srgbToLinear(emissiveMap.Sample(uvEmissive).rgb);
        }
        if ((material.flags & (1u << 23)) != 0u) {
            unlitEmissive *= clamp(fragColor.rgb, 0.0, 1.0);
        }
        // Under the camera frame (bit 5) compose owes exposure, tonemap and gamma, as
        // on the tail and the sky paths; applying them here too clipped an HDR emissive.
        bool linearHdrTarget = (lighting.flagsAndPad[0] & (1u << 5)) != 0u;
        if (vtFeatureEnabled(VT_FEATURE_MSDF_BIT)) {
            // MSDF: fill, outline and shadow composited premultiplied in linear, the
            // straight colour encoded at the end. The fill
            // is tone mapped unless compose owes it that (applyToneMap is a no-op for a UI
            // material); outline and shadow are not.
            float3 unlitColor = max(albedo.rgb + unlitEmissive, float3(0.0));
            float3 fill = linearHdrTarget ? unlitColor : applyToneMap(unlitColor);
            float4 color = float4(fill * albedo.a, albedo.a);
            float2 uvShadow = uvBase - material.msdfOutlineShadow.yz;
            float3 tsample = baseColorMap.Sample(uvBase).rgb;
            float3 ssample = baseColorMap.Sample(uvShadow).rgb;
            float sigDist = max(min(tsample.r, tsample.g), min(max(tsample.r, tsample.g), tsample.b));
            float sigDistShdw = max(min(ssample.r, ssample.g), min(max(ssample.r, ssample.g), ssample.b));
            float edge = 0.5 - 0.5 * material.msdfParams.y;
            // The page size comes in the block: textureSize() on a combined sampler does
            // not survive MoltenVK's translation.
            float2 unitRange = float2(material.msdfParams.x) / max(material.msdfParams.zw, float2(1.0));
            float screenPxRange = max(0.5 * dot(unitRange, 1.0 / max(fwidth(uvBase), float2(1e-6))), 2.5);
            float thickness = material.msdfOutlineShadow.x;
            float inside = clamp(screenPxRange * (sigDist - edge) + 0.5, 0.0, 1.0);
            float outline = clamp(screenPxRange * (sigDist + thickness - edge) + 0.5, 0.0, 1.0);
            float shadow = clamp(screenPxRange * (sigDistShdw + thickness - edge) + 0.5, 0.0, 1.0);
            float4 oc = material.msdfOutlineColor;
            float4 sc = material.msdfShadowColor;
            float4 tcolor = (outline > inside) ? outline * float4(oc.a * oc.rgb, oc.a) : float4(0.0);
            tcolor = lerp(tcolor, color, inside);
            float4 scolor = (shadow > outline) ? shadow * float4(sc.a * sc.rgb, sc.a) : tcolor;
            tcolor = lerp(scolor, tcolor, outline);
            float3 straight = tcolor.rgb / max(tcolor.a, 0.0001);
            outColor = float4(linearHdrTarget ? straight : pow(max(straight, float3(0.0)), float3(1.0 / 2.2)), tcolor.a);
            return outColor;
        }
        if (linearHdrTarget) {
            outColor = float4(max(albedo.rgb + unlitEmissive, float3(0.0)), albedo.a);
            return outColor;
        }
        float3 unlit = applyToneMap(albedo.rgb + unlitEmissive);   // exposure + tonemap
        outColor = float4(pow(max(unlit, float3(0.0)), float3(1.0 / 2.2)), albedo.a);
        return outColor;
    }

    // Metallic-roughness (glTF packs roughness in G, metallic in B).
    float metallic = material.metallicFactor;
    float roughness = material.roughnessFactor;
    if (vtFeatureEnabled(VT_FEATURE_METAL_ROUGHNESS_MAP_BIT)) {
        float4 mr = metalRoughMap.Sample(uvMetalRough);
        roughness *= mr.g;
        metallic *= mr.b;
    }
    // KHR_materials_pbrSpecularGlossiness: specular colour + glossiness in place of
    // metallic / roughness. The spec-gloss texture reuses the metal-rough slot (flags bit 21):
    // rgb the specular colour (sRGB), a the glossiness, read on both backends.
    float3 specGlossColor = float3(0.0);
    if (vtFeatureEnabled(VT_FEATURE_SPEC_GLOSS_BIT)) {
        specGlossColor = clamp(material.specGlossParams.rgb, 0.0, 1.0);
        float glossiness = clamp(material.specGlossParams.w, 0.0, 1.0);
        if ((material.flags & (1u << 21)) != 0u) {
            float4 sg = metalRoughMap.Sample(uvMetalRough);
            specGlossColor *= srgbToLinear(sg.rgb);
            glossiness *= sg.a;
        }
        metallic = 0.0;
        roughness = 1.0 - glossiness;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    // Gloss map: one channel scales the authored gloss FACTOR, the product is
    // inverted under glossInvert, and the result replaces the roughness above.
    if (material.mapChannelParams.y >= 0.0) {
        float4 glossSample = glossMap.Sample(materialSampler, uvGloss);
        float glossValue = material.mapChannelParams.x * glossSample[int(material.mapChannelParams.y)];
        if (material.glossMapParams.x > 0.5) {
            glossValue = 1.0 - glossValue;
        }
        roughness = clamp(1.0 - glossValue, 0.04, 1.0);
    }
    // Anisotropic GGX frame; the math is in common-brdf.slang. The direction is the
    // material's anisotropy rotation: (cos, sin) turning the
    // vertex tangent toward the vertex BITANGENT; the CPU folds the deprecated
    // negative strength in as rotation + 90 and uploads the magnitude. B is
    // cross(geometric normal, T), built from the TBN's own normal rather than the
    // normal-mapped one.
    float anisoIntensity = 0.0;
    float2 anisoAlpha = float2(1.0);
    float3 anisoT = float3(1.0, 0.0, 0.0);
    float3 anisoB = float3(0.0, 0.0, 1.0);
    if (vtFeatureEnabled(VT_FEATURE_ANISOTROPY_BIT)) {
        anisoIntensity = clamp(material.anisotropy, 0.0, 1.0);
        anisoAlpha = getAnisotropicAlpha(1.0 - roughness, anisoIntensity);
        float3 NgUnflipped = normalize(fragWorldNormal);
        float3 Ng = NgUnflipped;
        if (vtFeatureEnabled(VT_FEATURE_DOUBLE_SIDED_BIT) && !vtFrontFacing) {
            Ng = -Ng;
        }
        float3 Tv = fragWorldTangent.xyz;
        if (dot(Tv, Tv) >= 1e-6) {
            Tv = normalize(Tv);
            float3 Bv = normalize(cross(NgUnflipped, Tv)) * fragWorldTangent.w;
            anisoT = material.anisotropyParams.x * Tv + material.anisotropyParams.y * Bv;
        } else {
            // No tangent stream and no derivative-based fallback, so take any
            // tangent.
            float3 up = abs(Ng.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
            anisoT = normalize(cross(up, Ng));
        }
        anisoB = normalize(cross(Ng, anisoT));
    }
    metallic = clamp(metallic, 0.0, 1.0);

    // Ambient occlusion.
    float ao = 1.0;
    if (vtFeatureEnabled(VT_FEATURE_OCCLUSION_MAP_BIT)) {
        float occ = occlusionMap.Sample(uvOcclusion).r;
        ao = lerp(1.0, occ, material.occlusionStrength);
    }
    if (vtFeatureEnabled(VT_FEATURE_SSAO_BIT)) {
        // Lighting-mode SSAO: the screen-space factor multiplies the baked map
        // rather than replacing it, and is then occluded through exactly the same
        // ambient/direct/specular split. vtFragCoord and the sampler UV share the
        // top-left origin (the pass renders through the same negative-height
        // viewport), so both backends read the same texel.
        ao *= ssao.Sample(linearClampSampler, vtFragCoord.xy * lighting.screenInvResolution.xy).r;
    }

    // Geometric normal, flipped for back faces on double-sided materials. Every tangent
    // frame takes its bitangent from the UNFLIPPED normal and only its normal column is
    // flipped, so a back face sees the same tangent-space slopes as the front, mirrored
    // through the surface; deriving the bitangent from the flipped normal would mirror
    // the v slope of every map on back faces.
    float3 geometricNormal = normalize(fragWorldNormal);
    float3 N = geometricNormal;
    if (vtFeatureEnabled(VT_FEATURE_DOUBLE_SIDED_BIT) && !vtFrontFacing) {
        N = -N;
    }
    // The face's own normal, before any normal map: the clearcoat's base.
    const float3 faceNormal = N;

    // Tangent-space normal mapping (shadowParams2.w = global enable toggle).
    // The detail map overlays the base map, so both share one TBN transform.
    bool haveNormalMap = vtFeatureEnabled(VT_FEATURE_NORMAL_MAP_BIT) &&
        lighting.shadowParams2.w > 0.5;
    bool haveDetailNormal = vtFeatureEnabled(VT_FEATURE_DETAIL_NORMALS_BIT) &&
        lighting.shadowParams2.w > 0.5;
    if (haveNormalMap || haveDetailNormal) {
        float3 tn = float3(0.0, 0.0, 1.0);
        if (haveNormalMap) {
            tn = normalMap.Sample(uvNormal).xyz * 2.0 - 1.0;
            // normalScale blends the sampled
            // normal TOWARD FLAT, it does not scale xy. The two are only equal at
            // 0 and 1: scaling xy leaves z alone, so it steepens the normal's
            // slope where the mix flattens it, and a normal-mapped surface lit at
            // a grazing angle picks up several percent from the difference.
            tn = lerp(float3(0.0, 0.0, 1.0), tn, material.normalScale);
        }
        if (haveDetailNormal) {
            // Detail normal overlay. detailNormalScale blends the DETAIL map toward
            // flat, exactly as normalScale does for the base map, and the two normals
            // are then combined with a reoriented ("detail oriented") blend
            // rather than by adding their xy. Adding xy treats the detail's
            // slope as if the base were flat, so the combined slope is wrong wherever
            // the base is not; the reoriented blend rotates the detail into the base
            // normal's own frame, which is the whole point of it.
            float2 uvDetail = applyUvTransform(fragUV0,
                material.detailNormalTransform0, material.detailNormalTransform1);
            float3 detailSample = detailNormalMap.Sample(materialSampler, uvDetail).xyz * 2.0 - 1.0;
            detailSample = lerp(float3(0.0, 0.0, 1.0), detailSample,
                material.detailDisplacementParams.x);
            float3 n1 = tn + float3(0.0, 0.0, 1.0);
            float3 n2 = detailSample * float3(-1.0, -1.0, 1.0);
            tn = n1 * (dot(n1, n2) / n1.z) - n2;
        }
        // A mesh can reach here with no tangent stream at all: the glTF parser
        // leaves the attribute zero when the file carries none and the primitive
        // is not triangles, so CPU tangent generation cannot run. normalize() of a
        // zero vector is NaN, which would poison the shading normal and the whole
        // pixel — so skip normal mapping and keep the geometric normal. This port
        // has no derivative-based TBN fallback, so there is nothing else to fall
        // back to.
        float3 T = fragWorldTangent.xyz;
        if (dot(T, T) >= 1e-6) {
            T = normalize(T);
            // Bitangent from the geometric normal with the handedness sign carried
            // in tangent.w. The interpolated tangent and binormal are only
            // normalized, with no Gram-Schmidt — it
            // would move the shading normal wherever the
            // interpolated tangent leaves the surface plane.
            float3 B = normalize(cross(geometricNormal, T)) * fragWorldTangent.w;
            N = normalize(mul(tn, float3x3(T, B, N)));
        }
    }

    // Planar reflection DEPTH PASS: this camera exists only to produce the
    // per-pixel distance-from-plane map that scales the reflection blur, so all
    // lighting is skipped and the distance is written as grayscale. Placed here
    // rather than at the tail because nothing below affects the result — the
    // surface setup above is the last thing it shares with the lit path.
    if (vtFeatureEnabled(VT_FEATURE_PLANAR_REFLECTION_DEPTH_PASS_BIT)) {
        float distFromPlane = abs(fragWorldPos.y + lighting.reflectionDepthParams.x) /
            lighting.reflectionDepthParams.y;
        outColor = float4(float3(distFromPlane), 1.0);
        return outColor;
    }

    float3 V = normalize(lighting.cameraPosExposure.xyz - fragWorldPos);
    float NdotV = max(dot(N, V), 1e-4);

    // The non-metal F0 in the metalness workflow is computed on the CPU:
    // f0(IOR) x the metalness specular colour x the specularity
    // factor (KHR_materials_specular); 0.04 for the defaults.
    float3 dielectricF0 = vtFeatureEnabled(VT_FEATURE_SPEC_GLOSS_BIT)
        ? specGlossColor : material.metalnessSpecular.rgb;
    float3 F0 = lerp(dielectricF0, albedo.rgb, metallic);
    // VT_FEATURE_NO_SPECULAR: the material renders no
    // specular at all. A black F0 is not enough — the gloss-aware Fresnel still
    // reflects at grazing angles — so every specular term below is multiplied by
    // this. It is a specialization constant, so the zero folds away.
    float specularOn = vtFeatureEnabled(VT_FEATURE_NO_SPECULAR_BIT) ? 0.0 : 1.0;

    // Clearcoat (VT_FEATURE_CLEARCOAT): a thin dielectric coat (F0 = 0.04) over the
    // base, clearcoat intensity/gloss/normal maps included. The coat's direct and
    // indirect specular accumulate here and are composed energy-conservingly in the
    // tail (the base is dimmed by what the coat reflects), never added straight into
    // the colour.
    float ccSpecularity = material.clearCoatFactor;
    float ccGlossiness = 1.0 - clamp(material.clearCoatRoughness, 0.0, 1.0);
    // Clearcoat intensity and gloss maps read their GREEN channel,
    // at the base-colour UV. Both are gated on their flag bit, never
    // on the binding: an unbound slot holds a white fallback on one backend and
    // samples zero on another, so the flag is the one rule both follow.
    if ((material.flags & (1u << 14)) != 0u) {
        ccSpecularity *= clearCoatMap.Sample(materialSampler, uvBase)[int(material.clearCoatMapChannels.x)];
    }
    // The gloss map multiplies the AUTHORED factor, and an inverted coat gloss is
    // flipped after the multiply, so roughness = factor x texel under the flag.
    if ((material.flags & (1u << 15)) != 0u) {
        ccGlossiness = material.glossMapParams.z * clearCoatGlossMap.Sample(materialSampler, uvBase)[int(material.clearCoatMapChannels.y)];
        if (material.glossMapParams.y > 0.5) {
            ccGlossiness = 1.0 - ccGlossiness;
        }
    }
    ccGlossiness += 0.0000001; // prevent divide-by-zero
    // Clearcoat normal: the coat is a layer of its own, so it takes the face's normal,
    // NOT the base normal map, unless a coat normal map overrides it, blended toward
    // flat by clearCoatBumpiness exactly as the base normal map is by normalScale and
    // rotated into the world through the geometric tangent frame (no map without a
    // tangent stream, for the same reason as above).
    float3 ccNormalW = faceNormal;
    if ((material.flags & (1u << 16)) != 0u) {
        float3 ccSample = clearCoatNormalMap.Sample(materialSampler, uvNormal).xyz * 2.0 - 1.0;
        ccSample = normalize(lerp(float3(0.0, 0.0, 1.0), ccSample, material.clearCoatBumpiness));
        float3 ccT = fragWorldTangent.xyz;
        if (dot(ccT, ccT) >= 1e-6) {
            ccT = normalize(ccT);
            float3 ccB = normalize(cross(geometricNormal, ccT)) * fragWorldTangent.w;
            ccNormalW = normalize(mul(ccSample, float3x3(ccT, ccB, faceNormal)));
        }
    }
    // The coat's own GGX roughness for DIRECT light, from its gloss (map included).
    // The light loops take this and the coat normal, not the flat clearCoatRoughness
    // and the base normal, so a coat normal or gloss map changes the coat's
    // highlights as well as its reflections.
    float ccRoughness = max(1.0 - ccGlossiness, 0.04);
    float ccAlpha2 = ccRoughness * ccRoughness * ccRoughness * ccRoughness;
    float3 ccSpecularLight = float3(0.0);
    float3 ccReflection = float3(0.0);

    // Thin-film iridescence, computed once before the light loop and blended into
    // each Fresnel (direct, IBL, probe) by intensity. iridescenceParams:
    // x=intensity, y=IOR, z=thicknessMin(nm), w=thicknessMax(nm).
    float iridIntensity = 0.0;
    float3 iridFresnel = F0;
    if (vtFeatureEnabled(VT_FEATURE_IRIDESCENCE_BIT)) {
        iridIntensity = clamp(material.iridescenceParams.x, 0.0, 1.0);
        iridFresnel = getIridescence(max(dot(N, V), 0.001), F0,
            material.iridescenceParams.w, max(material.iridescenceParams.y, 1.001));
    }

    // Sheen (Charlie + Ashikhmin). The colour is authored in gamma space like every
    // other material colour; the alpha channel carries the sheen roughness.
    float3 sheenTint = srgbToLinear(material.sheenColor.rgb);
    float sheenRoughness = max(material.sheenColor.w, 0.04);
    float3 sheenSpecularDirect = float3(0.0);
    float3 sheenSpecularIndirect = float3(0.0);
    float3 diffuseAlbedo = albedo.rgb * (1.0 - metallic);

    // Lightmap bake (VT_FEATURE_LIGHTMAP_BAKE): the diffuse LIGHT reaching this texel,
    // accumulated without albedo — the runtime multiplies the sampled lightmap by the
    // surface's own diffuse colour. This shader folds albedo in at each accumulation
    // site, hence the parallel accumulator rather than a division at the end.
    float3 bakeDiffuseLight = float3(0.0);
    // Direct-only half of the same accumulator, for the accumulating bake passes.
    float3 bakeDirectLight = float3(0.0);

    float3 color = float3(0.0);
    // Dynamic refraction replaces the surface's diffuse with the refracted scene
    // but must keep specular, so the specular share is tracked separately.
    float3 directSpecular = float3(0.0);
    // Direct diffuse share (albedo folded in), so occludeDirect can occlude it after
    // the fact — see the occlusion block in forward-fragment-ambient.
    float3 directDiffuse = float3(0.0);

    // Shadow catcher accumulates directional shadow factors multiplicatively:
    // 1.0 = fully lit, 0.0 = fully shadowed.
    float dShadowCatcher = 1.0;

