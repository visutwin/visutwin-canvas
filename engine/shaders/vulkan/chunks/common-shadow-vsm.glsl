// ── EVSM_16F sampling (parity with common.metal getShadowVSM16) ──
// One-tailed Chebyshev upper bound with reduceLightBleeding(0.1), fed by
// exponentially-warped moments (c = 5.54). Cleared pixels (moments.z == 0)
// synthesize fully-lit moments.

float chebyshevUpperBound(vec2 moments, float mean, float minVariance) {
    float variance = max(moments.y - moments.x * moments.x, minVariance);
    float d = mean - moments.x;
    float pMax = variance / (variance + d * d);
    pMax = clamp((pMax - 0.1) / 0.9, 0.0, 1.0);   // reduceLightBleeding(0.1)
    return (mean <= moments.x) ? 1.0 : pMax;
}

float sampleShadowVSM16(vec2 uv, float receiverZ, float vsmBias) {
    const float VSM_EXPONENT = 5.54;
    vec3 moments = texture(shadowMap, uv).xyz;
    float warped = exp(VSM_EXPONENT * (2.0 * receiverZ - 1.0));
    vec2 stored = moments.xy + vec2(warped, warped * warped) * (1.0 - moments.z);
    float depthScale = vsmBias * VSM_EXPONENT * warped;
    return chebyshevUpperBound(stored, warped, depthScale * depthScale);
}

// Directional cascaded-shadow visibility (1 = lit, 0 = shadowed) for a world
// position, using view-space depth to pick the cascade.  Returns 1.0 when
// shadows are disabled or the point falls outside every cascade.
// shadowParams.x encodes the mode: 0 = off, 1 = PCF depth, 2 = EVSM moments.
// One cascade's visibility tap. Factored out of sampleDirectionalShadow so the
// cross-cascade blend below samples the neighbouring cascade through exactly the
// same filter — VSM, PCSS or PCF — rather than a second spelling of it.
float sampleCascadeVisibility(vec3 coord, int cascade) {
    if (lighting.shadowParams.x > 1.5) {
        return sampleShadowVSM16(coord.xy, coord.z, max(lighting.shadowParams.z, 1e-4));
    }
    if (vtFeatureEnabled(VT_FEATURE_PCSS_SHADOWS_BIT)) {
        return getShadowPCSSDirectional(coord.xy,
            coord.z - lighting.shadowParams.z,
            lighting.pcssCascadeRadii[cascade],
            lighting.pcssCascadeDepthRanges[cascade]);
    }
    return pcf3x3(shadowMap, coord.xy, coord.z - lighting.shadowParams.z);
}

float sampleDirectionalShadow(vec3 worldPos, float viewDepth, vec3 N, vec3 L) {
    if (lighting.shadowParams.x < 0.5) {
        return 1.0;
    }
    int cascadeCount = int(lighting.shadowParams.y);

    // Cascade = number of split distances the fragment is beyond.
    int cascade = 0;
    for (int i = 0; i < cascadeCount - 1; ++i) {
        if (viewDepth > lighting.shadowCascadeDistances[i]) {
            cascade = i + 1;
        }
    }

    // World-space normal bias, scaled by grazing angle to curb peter-panning
    // on directly-lit faces while offsetting shadow-acne on grazing ones.
    float ndl = clamp(dot(N, L), 0.0, 1.0);
    float sinAngle = sqrt(max(1.0 - ndl * ndl, 0.0));
    vec3 biased = worldPos + N * (lighting.shadowParams2.x * sinAngle);

    // Project into the cascade's atlas: the matrix bakes projection, view,
    // NDC→UV, and Z[0,1]; a perspective divide yields UV + depth directly.
    vec4 sc = lighting.shadowMatrices[cascade] * vec4(biased, 1.0);
    if (sc.w <= 0.0) {
        return 1.0;
    }
    vec3 coord = sc.xyz / sc.w;
    // No V flip: the cascade matrix bakes the Metal top-left atlas convention,
    // and the negative-height viewport used for every Vulkan pass (including
    // shadow renders) stores the map in exactly that orientation. A whole-atlas
    // 1-V flip here would sample the wrong cascade quadrant for any multi-
    // cascade layout.
    if (coord.x < 0.0 || coord.x > 1.0 || coord.y < 0.0 || coord.y > 1.0 ||
        coord.z < 0.0 || coord.z > 1.0) {
        return 1.0;
    }

    // PCF (mode 1) or EVSM Chebyshev (mode 2), scaled by shadow strength.
    // PCSS replaces the PCF tap when the shader is specialized for it — a light
    // has exactly one shadow type, so VSM and PCSS are mutually exclusive and
    // the ordering here matches the Metal chunk's #if/#elif chain.
    float visible = sampleCascadeVisibility(coord, cascade);
    float shadowFactor = mix(1.0, visible, lighting.shadowParams.w);

    // Cross-cascade blend, twin of the block in forward-fragment-lights.metal.
    // Without it each cascade ends in a hard line across the ground where the
    // filter radius changes; Metal has blended since cascades were added and this
    // backend never did, so the same scene showed seams on one backend only.
    float blendWidth = lighting.shadowParams2.y;
    if (blendWidth > 0.0 && cascade < cascadeCount - 1) {
        float cascadeFar = lighting.shadowCascadeDistances[cascade];
        float fade = clamp((cascadeFar - viewDepth) / blendWidth, 0.0, 1.0);
        if (fade < 1.0) {
            int nextCascade = cascade + 1;
            vec4 nsc = lighting.shadowMatrices[nextCascade] * vec4(biased, 1.0);
            float nextFactor = 1.0;
            if (nsc.w > 0.0) {
                vec3 ncoord = nsc.xyz / nsc.w;
                if (all(greaterThanEqual(ncoord, vec3(0.0))) &&
                    all(lessThanEqual(ncoord, vec3(1.0)))) {
                    nextFactor = mix(1.0, sampleCascadeVisibility(ncoord, nextCascade),
                        lighting.shadowParams.w);
                }
            }
            shadowFactor = mix(nextFactor, shadowFactor, fade);
        }
    }

    // Fade the shadow out over the last tenth of the cascade range, so geometry
    // does not step from shadowed to lit at the edge of the furthest cascade.
    float maxDist = lighting.shadowCascadeDistances[cascadeCount - 1];
    float fadeStart = maxDist * 0.9;
    if (viewDepth > fadeStart && maxDist > fadeStart) {
        shadowFactor = mix(shadowFactor, 1.0,
            clamp((viewDepth - fadeStart) / (maxDist - fadeStart), 0.0, 1.0));
    }
    return shadowFactor;
}

// Spot-light 2D shadow visibility (1 = lit, 0 = shadowed).  Mirrors the
// directional path but with a single per-light matrix, bias, and intensity.
float sampleSpotShadow(int slot, vec3 worldPos, vec3 N, vec3 L) {
    mat4 m  = (slot == 0) ? lighting.localShadowMatrix0 : lighting.localShadowMatrix1;
    vec4 sp = (slot == 0) ? lighting.localShadowParams0 : lighting.localShadowParams1;

    // World-space normal bias, scaled by grazing angle (matches Metal).
    float ndl = clamp(dot(N, L), 0.0, 1.0);
    float sinAngle = sqrt(max(1.0 - ndl * ndl, 0.0));
    vec3 biased = worldPos + N * (sp.y * sinAngle);

    vec4 sc = m * vec4(biased, 1.0);
    if (sc.w <= 0.0) {
        return 1.0;
    }
    vec3 coord = sc.xyz / sc.w;
    // No V flip — same reasoning as the CSM path: the spot matrix bakes the
    // Metal orientation and the negative-height viewport reproduces it.
    if (coord.x < 0.0 || coord.x > 1.0 || coord.y < 0.0 || coord.y > 1.0 ||
        coord.z < 0.0 || coord.z > 1.0) {
        return 1.0;
    }

    float receiver = coord.z - sp.x;
    // PCSS is a runtime branch here (no extra specialization): a non-zero
    // search area means this slot's light uses SHADOW_PCSS_32F.
    vec4 pc = (slot == 0) ? lighting.localShadowPcss0 : lighting.localShadowPcss1;
    float visible;
    if (pc.x > 0.0) {
        visible = (slot == 0)
            ? getShadowPCSSSpot(localShadowMap0, coord.xy, receiver, pc.x, pc.y, pc.z)
            : getShadowPCSSSpot(localShadowMap1, coord.xy, receiver, pc.x, pc.y, pc.z);
    } else {
        visible = (slot == 0)
            ? pcf3x3(localShadowMap0, coord.xy, receiver)
            : pcf3x3(localShadowMap1, coord.xy, receiver);
    }
    // Local shadows blend toward (1 - intensity) when occluded.
    return mix(1.0 - clamp(sp.z, 0.0, 1.0), 1.0, visible);
}

// Omni (point) light cubemap shadow visibility.  The light→fragment direction
// selects the cubemap face; the stored depth is the perspective-projected
// distance along the dominant axis, reconstructed here from the linear
// distance to match the shadow render's projection.
float sampleOmniShadow(int slot, vec3 worldPos, vec3 lightPos) {
    vec4 op = (slot == 0) ? lighting.omniShadowParams0 : lighting.omniShadowParams1;
    float nearV = op.x, farV = op.y, bias = op.z, intensity = op.w;

    vec3 dir = worldPos - lightPos;

    // PCSS is a runtime branch here (no extra specialization): a non-zero
    // search area means this slot's light uses SHADOW_PCSS_32F.
    vec4 pc = (slot == 0) ? lighting.localShadowPcss0 : lighting.localShadowPcss1;
    float visible;
    if (pc.x > 0.0) {
        visible = (slot == 0)
            ? getShadowPCSSOmni(omniShadowCube0, dir, pc.x, pc.y, pc.z, bias)
            : getShadowPCSSOmni(omniShadowCube1, dir, pc.x, pc.y, pc.z, bias);
    } else {
        float d = max(abs(dir.x), max(abs(dir.y), abs(dir.z)));
        // Relative bias applied before the projection — see the matching comment in
        // forward-fragment-lights.metal: perspective depth is crushed against 1.0 at
        // these ranges, so a fixed post-projection offset swallows real shadows.
        float dBiased = d * (1.0 - bias);
        float denom = (farV - nearV) * dBiased;
        float compareValue = farV * (dBiased - nearV) / max(denom, 1e-6);

        // Four diagonal taps around the sample direction (upstream shadowPCF3's
        // omni form). Metal gets bilinear filtering for free from a hardware
        // sample_compare; a plain texture().r fetch here does not, so a single tap
        // left point-light shadows visibly stair-stepped on this backend alone.
        vec3 t0 = normalize(dir);
        vec3 up = abs(t0.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        vec3 tx = normalize(cross(up, t0));
        vec3 ty = cross(t0, tx);
        const float kOmniTapRadius = 0.0035;   // radians of cone half-angle
        visible = 0.0;
        for (int i = 0; i < 4; ++i) {
            vec2 o = (i == 0) ? vec2(-1.0, -1.0)
                   : (i == 1) ? vec2( 1.0, -1.0)
                   : (i == 2) ? vec2(-1.0,  1.0) : vec2(1.0, 1.0);
            vec3 tapDir = normalize(t0 + (tx * o.x + ty * o.y) * kOmniTapRadius);
            float tapOccluder = (slot == 0) ? texture(omniShadowCube0, tapDir).r
                                            : texture(omniShadowCube1, tapDir).r;
            visible += (compareValue <= tapOccluder) ? 0.25 : 0.0;
        }
    }
    return mix(1.0 - clamp(intensity, 0.0, 1.0), 1.0, visible);
}

