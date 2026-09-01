// ── PCSS: contact-hardening soft shadows (parity with common-shadow-pcss.metal) ──
// Vogel-disk blocker search sizes a per-fragment penumbra, then a second disk
// pass filters at that radius.  Every shadow map here is already bound through
// a NEAREST clamp-to-edge non-comparison sampler, which is exactly the raw
// sampler the Metal chunk uses.  The directional path is selected by the
// VT_FEATURE_PCSS_SHADOWS specialization constant (mirroring Metal's variant);
// the spot/omni paths branch at runtime on a non-zero search area, mirroring
// Metal's uniform branch.

// Metal uses fmod here; for the non-negative gl_FragCoord.xy inputs GLSL's
// floor-based mod is identical.
float pcssFractSinRand(vec2 uv) {
    const float a = 12.9898, b = 78.233, c = 43758.5453;
    float dt = dot(uv, vec2(a, b));
    return fract(sin(mod(dt, PI)) * c);
}

// Vogel disk: point `id` of `invCount` = 1/count, rotated by `initialAngle`.
vec2 pcssDiskSample(float id, float invCount, float initialAngle) {
    const float GOLDEN_ANGLE = 2.399963;
    float r = sqrt((id + 0.5) * invCount);
    float theta = id * GOLDEN_ANGLE + initialAngle;
    return vec2(r * cos(theta), r * sin(theta));
}

// Directional PCSS.  `orthoRadius` / `depthRange` are the cascade's shadow-camera
// world half-extent and caster depth span; `receiverDepth` arrives biased.
float getShadowPCSSDirectional(vec2 uv, float receiverDepth,
                               float orthoRadius, float depthRange) {
    // Clamp so cleared texels (depth 1) are not treated as blockers when the
    // receiver sits outside the tightened cascade depth range.
    float receiverDepthClamped = min(receiverDepth, 0.9999);
    float initialAngle = pcssFractSinRand(gl_FragCoord.xy) * 2.0 * PI;

    // A zero filter count would divide the accumulated visibility by zero and
    // poison the frame with NaN; the renderer always sends 16.
    int shadowSamples = max(int(lighting.pcssParams.x), 1);
    int blockerSamples = int(lighting.pcssParams.y);
    float penumbraSize = lighting.pcssParams.z;
    float penumbraFalloff = lighting.pcssParams.w;

    float worldPerUv = 2.0 * orthoRadius;

    float filterRadius;
    if (blockerSamples > 0) {
        // The blocker search radius bounds the largest possible penumbra.
        float searchWidthUv = (penumbraSize * depthRange) / worldPerUv;
        float invBlockers = 1.0 / float(blockerSamples);
        float blockerSum = 0.0;
        int numBlockers = 0;
        for (int i = 0; i < blockerSamples; ++i) {
            vec2 sampleUv = uv +
                pcssDiskSample(float(i), invBlockers, initialAngle) * searchWidthUv;
            float occluder = texture(shadowMap, sampleUv).r;
            if (occluder < receiverDepthClamped) {
                blockerSum += occluder;
                numBlockers++;
            }
        }
        if (numBlockers < 1) {
            return 1.0;
        }
        float avgBlockerDepth = blockerSum / float(numBlockers);

        // World-space penumbra with shape control: reaches penumbraSize *
        // depthRange when the blocker sits at the far end of the caster range.
        float worldDist = max((receiverDepth - avgBlockerDepth) * depthRange, 0.0);
        float t = clamp(worldDist / depthRange, 0.0, 1.0);
        float shape = 1.0 - pow(1.0 - t, penumbraFalloff);
        filterRadius = (shape * penumbraSize * depthRange) / worldPerUv;
    } else {
        // Constant filter size — no contact hardening.
        filterRadius = penumbraSize / worldPerUv;
    }

    float invSamples = 1.0 / float(shadowSamples);
    float sum = 0.0;
    for (int i = 0; i < shadowSamples; ++i) {
        vec2 sampleUv = uv +
            pcssDiskSample(float(i), invSamples, initialAngle) * filterRadius;
        sum += step(receiverDepthClamped, texture(shadowMap, sampleUv).r);
    }
    return sum * invSamples;
}

// Local-light PCSS works in linear view distance, so every tap is linearized.
const int PCSS_LOCAL_SAMPLE_COUNT = 16;

float pcssLinearizeDepth(float z, float nearClip, float farClip) {
    return (nearClip * farClip) / max(farClip - z * (farClip - nearClip), 1e-6);
}

// Stored omni depth (far*(d-near)/((far-near)*d)) → normalized distance d/far.
float pcssCubeStoredToLinear(float stored, float nearClip, float farClip) {
    float d = (farClip * nearClip) / max(farClip - stored * (farClip - nearClip), 1e-6);
    return d / farClip;
}

// Vogel sphere (upstream vogelSphere: radius = weight = i/count).
vec3 pcssVogelSphere(int sampleIndex, int count, float phi) {
    const float GOLDEN_ANGLE = 2.4;
    float theta = float(sampleIndex) * GOLDEN_ANGLE + phi;
    float weight = float(sampleIndex) / float(count);
    return vec3(cos(theta) * weight, weight, sin(theta) * weight);
}

// Spot PCSS.  `searchArea` is the blocker-search radius in shadow-map UV
// (penumbraSize / resolution * fovRatio, packed CPU-side); `receiverZ` arrives
// with the depth bias already applied.
float getShadowPCSSSpot(sampler2D tex, vec2 uv, float receiverZ,
                        float searchArea, float nearClip, float farClip) {
    float receiverDepth = pcssLinearizeDepth(receiverZ, nearClip, farClip);
    float initialAngle = pcssFractSinRand(gl_FragCoord.xy) * 2.0 * PI;
    const float invCount = 1.0 / float(PCSS_LOCAL_SAMPLE_COUNT);

    float blockerSum = 0.0;
    int numBlockers = 0;
    for (int i = 0; i < PCSS_LOCAL_SAMPLE_COUNT; ++i) {
        vec2 sampleUv = uv +
            pcssDiskSample(float(i), invCount, initialAngle) * searchArea;
        float depthLin = pcssLinearizeDepth(texture(tex, sampleUv).r, nearClip, farClip);
        if (depthLin < receiverDepth) {
            blockerSum += depthLin;
            numBlockers++;
        }
    }
    if (numBlockers < 1) {
        return 1.0;
    }
    float avgBlockerDepth = blockerSum / float(numBlockers);

    // upstream: filterRadius = (receiver - avgBlocker) / 3 * searchArea
    float filterRadius = ((receiverDepth - avgBlockerDepth) / 3.0) * searchArea;

    float sum = 0.0;
    for (int i = 0; i < PCSS_LOCAL_SAMPLE_COUNT; ++i) {
        vec2 sampleUv = uv +
            pcssDiskSample(float(i), invCount, initialAngle) * filterRadius;
        float depthLin = pcssLinearizeDepth(texture(tex, sampleUv).r, nearClip, farClip);
        sum += step(receiverDepth, depthLin);
    }
    return sum * invCount;
}

// Omni PCSS: Vogel-sphere direction perturbation on the depth cube, blocker
// search and filter in normalized linear distance.  `lightDir` is the
// unnormalized light → fragment vector.
float getShadowPCSSOmni(samplerCube tex, vec3 lightDir, float searchArea,
                        float nearClip, float farClip, float bias) {
    float receiverDepth = length(lightDir) / farClip - bias;
    vec3 lightDirNorm = normalize(lightDir);
    float phi = pcssFractSinRand(gl_FragCoord.xy) * 2.0 * PI;
    const float invCount = 1.0 / float(PCSS_LOCAL_SAMPLE_COUNT);

    float blockerSum = 0.0;
    int numBlockers = 0;
    for (int i = 0; i < PCSS_LOCAL_SAMPLE_COUNT; ++i) {
        vec3 sampleDir = normalize(lightDirNorm +
            pcssVogelSphere(i, PCSS_LOCAL_SAMPLE_COUNT, phi) * searchArea);
        float depthLin = pcssCubeStoredToLinear(texture(tex, sampleDir).r, nearClip, farClip);
        if (depthLin < receiverDepth) {
            blockerSum += depthLin;
            numBlockers++;
        }
    }
    if (numBlockers < 1) {
        return 1.0;
    }
    float avgBlockerDepth = blockerSum / float(numBlockers);

    // upstream: filterRadius = (receiver - blocker) / blocker * searchArea
    float filterRadius =
        ((receiverDepth - avgBlockerDepth) / max(avgBlockerDepth, 1e-4)) * searchArea;

    float sum = 0.0;
    for (int i = 0; i < PCSS_LOCAL_SAMPLE_COUNT; ++i) {
        vec3 sampleDir = normalize(lightDirNorm +
            pcssVogelSphere(i, PCSS_LOCAL_SAMPLE_COUNT, phi) * filterRadius);
        float depthLin = pcssCubeStoredToLinear(texture(tex, sampleDir).r, nearClip, farClip);
        sum += step(receiverDepth, depthLin);
    }
    return sum * invCount;
}

