void main() {
    // Skybox: sample the environment along the view direction and output it
    // directly — no surface lighting.  The sky mesh is centered on the camera,
    // so the world-space position relative to the camera is the view ray.
    if (vtFeatureEnabled(VT_FEATURE_SKYBOX_BIT)) {
        // Dome projection: view direction from the dome center rather than
        // the camera so the flattened bottom hemisphere reads as a ground
        // plane (tripod projection). Mirrors forward-fragment-head.metal.
        uint skyFlags = uint(lighting.skyParams2.w + 0.5);
        vec3 dir = ((skyFlags & 2u) != 0u)
            ? normalize(fragWorldPos - lighting.skyParams2.xyz)
            : normalize(fragWorldPos - lighting.cameraPosExposure.xyz);
        vec3 sky;
        if (vtFeatureEnabled(VT_FEATURE_ATMOSPHERE_BIT)) {
            // Nishita scattering replaces the cubemap/atlas sky entirely, and
            // takes precedence over both — mirroring the #if/#elif ordering in
            // forward-fragment-head.metal.
            //
            // DEVIATION: Metal derives the view ray from the pre-transform local
            // vertex position (carried in its worldNormal varying) to dodge
            // float32 cancellation when camera distance and planet radius are
            // both ~1e7 m. This backend's sky varyings do not carry that, so it
            // reuses `dir` like the other sky paths here. Fine at scene scale;
            // revisit if this backend is ever driven at globe scale.
            sky = nishitaScatter(dir);
        } else if ((skyFlags & 1u) != 0u) {
            // High-res skybox cubemap (negated X matches the engine's atlas
            // lookup handedness — same as the Metal SKY_CUBEMAP path).
            float intensity = max(lighting.envParams.x, 0.0);
            sky = decodeEnv(texture(skyboxCube, vec3(-dir.x, dir.y, dir.z))) * intensity;
        } else if (vtFeatureEnabled(VT_FEATURE_ENV_ATLAS_BIT) &&
                   lighting.envParams.y > 0.5) {
            float intensity = max(lighting.envParams.x, 0.0);
            sky = decodeEnv(texture(envAtlas, mapRoughnessUv(dirToEquirect(dir),
                                    max(lighting.envParams.w, 0.0)))) * intensity;
        } else {
            sky = material.baseColor.rgb;
        }
        // Under the camera-frame path (bit 5 of flagsAndPad[0]) the sky, like
        // every other forward draw, must leave linear HDR for compose to expose,
        // tonemap and gamma-encode. Without this the sky was written already
        // gamma-encoded into the linear HDR target and compose encoded it a
        // second time, which is what made every camera-frame scene here brighter
        // and bluer than Metal. Mirrors the same check in each sky path of
        // forward-fragment-head.metal.
        if ((lighting.flagsAndPad[0] & (1u << 5)) != 0u) {
            outColor = vec4(max(sky, vec3(0.0)), 1.0);
            return;
        }
        sky *= lighting.cameraPosExposure.w;            // exposure
        sky = applyToneMap(sky);
        outColor = vec4(pow(max(sky, vec3(0.0)), vec3(1.0 / 2.2)), 1.0); // display-gamma encode
        return;
    }

    // Per-map UVs: select the UV set by flag bit, then apply that map's own
    // transform — previously the base-color transform was applied to every map.
    vec2 uvBase = applyUvTransform(
        ((material.flags & FLAG_BASE_UV1) != 0u) ? fragUV1 : fragUV0,
        material.baseColorTransform0, material.baseColorTransform1);
    vec2 uvNormal = applyUvTransform(
        ((material.flags & FLAG_NORMAL_UV1) != 0u) ? fragUV1 : fragUV0,
        material.normalTransform0, material.normalTransform1);
    vec2 uvMetalRough = applyUvTransform(
        ((material.flags & FLAG_METALROUGH_UV1) != 0u) ? fragUV1 : fragUV0,
        material.metalRoughTransform0, material.metalRoughTransform1);
    vec2 uvOcclusion = applyUvTransform(
        ((material.flags & FLAG_OCCLUSION_UV1) != 0u) ? fragUV1 : fragUV0,
        material.occlusionTransform0, material.occlusionTransform1);
    vec2 uvEmissive = applyUvTransform(
        ((material.flags & FLAG_EMISSIVE_UV1) != 0u) ? fragUV1 : fragUV0,
        material.emissiveTransform0, material.emissiveTransform1);

    // Parallax occlusion mapping offsets all material UVs before any sampling.
    // The flags bit matters as well as the feature gate: an unbound image reads
    // as white here, which would offset UVs on a material that has no height map.
    if (vtFeatureEnabled(VT_FEATURE_PARALLAX_BIT) &&
        (material.flags & FLAG_HAS_HEIGHTMAP) != 0u &&
        material.heightMapFactor > 0.0) {
        vec3 nGeom = normalize(fragWorldNormal);
        vec3 vPar = normalize(lighting.cameraPosExposure.xyz - fragWorldPos);
        vec3 tPar = fragWorldTangent.xyz;
        if (dot(tPar, tPar) >= 1e-6) {
            tPar = normalize(tPar);
            vec3 bPar = normalize(cross(nGeom, tPar)) * fragWorldTangent.w;
            // dot(basis, V) transforms the world view vector into tangent space.
            vec3 viewDirTS = normalize(
                vec3(dot(tPar, vPar), dot(bPar, vPar), dot(nGeom, vPar)));
            vec2 uvDelta = parallaxOcclusionMap(
                uvBase, viewDirTS, material.heightMapFactor) - uvBase;
            uvBase       += uvDelta;
            uvNormal     += uvDelta;
            uvMetalRough += uvDelta;
            uvOcclusion  += uvDelta;
            uvEmissive   += uvDelta;
        }
    }

    vec4 baseSample = vtFeatureEnabled(VT_FEATURE_BASE_COLOR_MAP_BIT)
        ? texture(baseColorMap, uvBase) : vec4(1.0);
    // fragColor is vec4(1) except for the vertex-color / point-cloud vertex
    // variants, which feed the mesh's per-vertex color through.
    // Upstream splits this into `diffuseVertexColor` / `emissiveVertexColor`:
    // flag bit 28 takes the color OFF the diffuse lane (so a zero flags word keeps
    // the old behaviour) and bit 23 routes it to emissive instead. Alpha always
    // modulates opacity.
    bool diffuseVertexColorOff = (material.flags & (1u << 28)) != 0u;
    vec4 vertexTint = diffuseVertexColorOff ? vec4(1.0, 1.0, 1.0, fragColor.a) : fragColor;
    // Material colours are authored in gamma space (upstream's convention), so the
    // factor AND the texture sample are decoded to linear before lighting. This
    // backend did neither, which left every surface brighter and less saturated
    // than Metal. Outside the camera-frame path the forward tonemap compressed the
    // difference to a few percent, which is why it read as a camera-frame bug.
    vec3 baseLinear = srgbToLinear(material.baseColor.rgb) * srgbToLinear(baseSample.rgb);
    vec4 albedo = vec4(baseLinear * vertexTint.rgb,
        material.baseColor.a * baseSample.a * vertexTint.a);

    // DEBUGPASS_LIGHTING (upstream debug-process-frontend.js): neutralize albedo
    // before it feeds diffuseAlbedo/F0 so the lit result shows the lighting
    // alone rather than the texture. Unlike every other debug mode this one does
    // NOT replace the output — it falls through to the normal lit path, and so
    // still receives fog and tonemapping. Mirrors forward-fragment-surface.metal.
    if (vtFeatureEnabled(VT_FEATURE_DEBUG_PASS_BIT) &&
        lighting.flagsAndPad[1] == VT_DEBUGPASS_LIGHTING) {
        albedo.rgb = vec3(0.5);
    }

    if (vtFeatureEnabled(VT_FEATURE_ALPHA_TEST_BIT) &&
        albedo.a < material.alphaCutoff) {
        discard;
    }

    // Opacity dithering (upstream opacity-dither.js): screen-space ordered
    // dither turns partial opacity into a discard pattern so transparency
    // renders in the opaque pass with correct depth. The matrix is chosen per
    // material via flags bits 25-27 (DitherMode) — a runtime value, not a shader
    // variant, so switching matrices needs no recompile. DEVIATION: no
    // blue-noise / IGN variants and no per-frame jitter (static pattern;
    // upstream jitters for TAA convergence).
    if (vtFeatureEnabled(VT_FEATURE_OPACITY_DITHER_BIT)) {
        // Upstream's alphaDither decouples the two strengths: opacity keeps driving the
        // alpha blend while this value alone drives the dither density. Negative means
        // unset, which restores the coupled behaviour every material had before.
        float ditherStrength = material.dispersionParams.y;
        bool hasAlphaDither = ditherStrength >= 0.0;
        float ditherAlpha = hasAlphaDither ? ditherStrength : albedo.a;

        if (ditherDiscards((material.flags >> 25) & 0x7u, gl_FragCoord.xy, ditherAlpha)) {
            discard;
        }

        // Coupled (legacy) use is an opaque-pass technique, so forcing alpha keeps the
        // target's alpha channel clean. Decoupled use is upstream's blend-AND-dither case,
        // where alpha must survive to drive the blend.
        if (!hasAlphaDither) {
            albedo.a = 1.0;
        }
    }

    // Point-cloud (unlit) path: the point vertex variant writes a zero world
    // normal as its sentinel — no surface lighting, just exposure + tonemap +
    // gamma on the tinted color (mirrors Metal's unlit point shader).
    if (vtFeatureEnabled(VT_FEATURE_UNLIT_BIT) ||
        dot(fragWorldNormal, fragWorldNormal) < 1e-6) {
        // Emissive still contributes here: upstream reaches this path through
        // `useLighting = false`, which drops the lights but keeps the emissive
        // lane (that is how its decal material draws at all).
        vec3 unlitEmissive = material.emissiveColor.rgb;
        if (vtFeatureEnabled(VT_FEATURE_EMISSIVE_MAP_BIT)) {
            unlitEmissive *= texture(emissiveMap, uvEmissive).rgb;
        }
        if ((material.flags & (1u << 23)) != 0u) {
            unlitEmissive *= clamp(fragColor.rgb, 0.0, 1.0);
        }
        vec3 unlit = (albedo.rgb + unlitEmissive) * lighting.cameraPosExposure.w;
        unlit = applyToneMap(unlit);
        outColor = vec4(pow(max(unlit, vec3(0.0)), vec3(1.0 / 2.2)), albedo.a);
        return;
    }

    // Metallic-roughness (glTF packs roughness in G, metallic in B).
    float metallic = material.metallicFactor;
    float roughness = material.roughnessFactor;
    if (vtFeatureEnabled(VT_FEATURE_METAL_ROUGHNESS_MAP_BIT)) {
        vec4 mr = texture(metalRoughMap, uvMetalRough);
        roughness *= mr.g;
        metallic *= mr.b;
    }
    if (vtFeatureEnabled(VT_FEATURE_SPEC_GLOSS_BIT)) {
        roughness = 1.0 - material.specGlossParams.w;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    if (vtFeatureEnabled(VT_FEATURE_ANISOTROPY_BIT)) {
        // Energy-preserving scalar approximation until the tangent-aligned
        // GGX sampling path is shared with Metal.
        roughness = clamp(roughness * (1.0 - 0.35 * abs(material.anisotropy)),
            0.04, 1.0);
    }
    metallic = clamp(metallic, 0.0, 1.0);

    // Ambient occlusion.
    float ao = 1.0;
    if (vtFeatureEnabled(VT_FEATURE_OCCLUSION_MAP_BIT)) {
        float occ = texture(occlusionMap, uvOcclusion).r;
        ao = mix(1.0, occ, material.occlusionStrength);
    }

    // Geometric normal, flipped for back faces on double-sided materials.
    vec3 N = normalize(fragWorldNormal);
    if (vtFeatureEnabled(VT_FEATURE_DOUBLE_SIDED_BIT) && !gl_FrontFacing) {
        N = -N;
    }

    // Tangent-space normal mapping (shadowParams2.w = global enable toggle).
    // The detail map overlays the base map, so both share one TBN transform.
    bool haveNormalMap = vtFeatureEnabled(VT_FEATURE_NORMAL_MAP_BIT) &&
        lighting.shadowParams2.w > 0.5;
    bool haveDetailNormal = vtFeatureEnabled(VT_FEATURE_DETAIL_NORMALS_BIT) &&
        lighting.shadowParams2.w > 0.5;
    if (haveNormalMap || haveDetailNormal) {
        vec3 tn = vec3(0.0, 0.0, 1.0);
        if (haveNormalMap) {
            tn = texture(normalMap, uvNormal).xyz * 2.0 - 1.0;
            tn.xy *= material.normalScale;
        }
        if (haveDetailNormal) {
            // UDN blend: the detail map's xy perturbation is scaled by
            // detailNormalScale and added on top of the base normal (or the flat
            // normal when no base map is bound).
            vec2 uvDetail = applyUvTransform(fragUV0,
                material.detailNormalTransform0, material.detailNormalTransform1);
            vec3 detailSample = texture(detailNormal, uvDetail).xyz * 2.0 - 1.0;
            detailSample.xy *= material.detailDisplacementParams.x;
            tn = normalize(vec3(tn.xy + detailSample.xy, tn.z));
        }
        // A mesh can reach here with no tangent stream at all: the glTF parser
        // leaves the attribute zero when the file carries none and the primitive
        // is not triangles, so CPU tangent generation cannot run. normalize() of a
        // zero vector is NaN, which would poison the shading normal and the whole
        // pixel — so skip normal mapping and keep the geometric normal, which is
        // what the Metal chunk does. This port has no derivative-based TBN
        // fallback (upstream's TBN.js), so there is nothing else to fall back to.
        vec3 T = fragWorldTangent.xyz;
        if (dot(T, T) >= 1e-6) {
            T = normalize(T);
            // Re-orthonormalize (Gram-Schmidt) and build the bitangent with the
            // handedness sign carried in tangent.w.
            T = normalize(T - N * dot(N, T));
            vec3 B = cross(N, T) * fragWorldTangent.w;
            N = normalize(mat3(T, B, N) * tn);
        }
    }

    // Planar reflection DEPTH PASS: this camera exists only to produce the
    // per-pixel distance-from-plane map that scales the reflection blur, so all
    // lighting is skipped and the distance is written as grayscale. Placed here
    // rather than at the tail (where forward-fragment-tail.metal has it) because
    // nothing below affects the result — the surface setup above is the last
    // thing it shares with the lit path.
    if (vtFeatureEnabled(VT_FEATURE_PLANAR_REFLECTION_DEPTH_PASS_BIT)) {
        float distFromPlane = abs(fragWorldPos.y + lighting.reflectionDepthParams.x) /
            lighting.reflectionDepthParams.y;
        outColor = vec4(vec3(distFromPlane), 1.0);
        return;
    }

    vec3 V = normalize(lighting.cameraPosExposure.xyz - fragWorldPos);
    float NdotV = max(dot(N, V), 1e-4);

    vec3 dielectricF0 = vtFeatureEnabled(VT_FEATURE_SPEC_GLOSS_BIT)
        ? material.specGlossParams.rgb : vec3(0.04);
    vec3 F0 = mix(dielectricF0, albedo.rgb, metallic);
    if (vtFeatureEnabled(VT_FEATURE_IRIDESCENCE_BIT)) {
        float film = material.iridescenceParams.x;
        float phase = material.iridescenceParams.z * 0.01;
        vec3 filmTint = 0.5 + 0.5 * cos(phase + vec3(0.0, 2.094, 4.189));
        F0 = mix(F0, filmTint, clamp(film, 0.0, 1.0));
    }
    vec3 diffuseAlbedo = albedo.rgb * (1.0 - metallic);

    // Lightmap bake (VT_FEATURE_LIGHTMAP_BAKE): the diffuse LIGHT reaching this texel,
    // accumulated without albedo — the runtime multiplies the sampled lightmap by the
    // surface's own diffuse colour. This shader folds albedo in at each accumulation
    // site, hence the parallel accumulator rather than a division at the end.
    vec3 bakeDiffuseLight = vec3(0.0);
    // Direct-only half of the same accumulator, for the accumulating bake passes.
    vec3 bakeDirectLight = vec3(0.0);

    vec3 color = vec3(0.0);
    // Dynamic refraction replaces the surface's diffuse with the refracted scene
    // but must keep specular, so the specular share is tracked separately.
    vec3 directSpecular = vec3(0.0);

    // Shadow catcher accumulates directional shadow factors multiplicatively:
    // 1.0 = fully lit, 0.0 = fully shadowed. Mirrors the declaration in
    // forward-fragment-surface.metal.
    float dShadowCatcher = 1.0;

