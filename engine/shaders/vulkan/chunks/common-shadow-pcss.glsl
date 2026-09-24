// ── Directional shadow slots ──
// Two directional lights can be shadowed (ShadowParams::kMaxDirectionalShadows);
// the light's coneParams.w picks the slot. These accessors return a slot's
// uniforms, and directionalShadowTexel reads its map. The maps are SEPARATE
// images, and a sampler built over one may only appear where it is used — never
// as a call argument — so the slot is chosen at each tap. Each map reads through
// the filter its own sampler has (ShadowMap::create): EVSM moments linear,
// depth nearest. The shadow type is scene-wide, so slot 0's mode decides.

mat4 directionalShadowMatrix(int slot, int cascade) {
    return slot == 0 ? lighting.shadowMatrices[cascade] : lighting.dirShadow1Matrices[cascade];
}
vec4 directionalShadowDistances(int slot) {
    return slot == 0 ? lighting.shadowCascadeDistances : lighting.dirShadow1CascadeDistances;
}
// enabled, numCascades, depthBias, strength
vec4 directionalShadowParams(int slot) {
    return slot == 0 ? lighting.shadowParams : lighting.dirShadow1Params;
}
// normalBias, cascadeBlend (slot 0's zw carry unrelated values)
vec2 directionalShadowParams2(int slot) {
    return slot == 0 ? lighting.shadowParams2.xy : lighting.dirShadow1Params2.xy;
}
vec4 directionalShadowPcss(int slot) {
    return slot == 0 ? lighting.pcssParams : lighting.dirShadow1PcssParams;
}
vec4 directionalShadowPcssRadii(int slot) {
    return slot == 0 ? lighting.pcssCascadeRadii : lighting.dirShadow1PcssCascadeRadii;
}
vec4 directionalShadowPcssDepthRanges(int slot) {
    return slot == 0 ? lighting.pcssCascadeDepthRanges : lighting.dirShadow1PcssCascadeDepthRanges;
}

vec4 directionalShadowTexel(int slot, vec2 uv) {
    if (lighting.shadowParams.x > 1.5) {
        return slot == 0 ? textureLod(sampler2D(shadowMapImage, linearClampSampler), uv, 0.0)
                         : textureLod(sampler2D(shadowMapImage1, linearClampSampler), uv, 0.0);
    }
    return slot == 0 ? textureLod(sampler2D(shadowMapImage, nearestClampSampler), uv, 0.0)
                     : textureLod(sampler2D(shadowMapImage1, nearestClampSampler), uv, 0.0);
}

// A depth map's texel (PCF, PCSS): always the nearest sampler.
float directionalShadowDepth(int slot, vec2 uv) {
    return slot == 0 ? textureLod(sampler2D(shadowMapImage, nearestClampSampler), uv, 0.0).r
                     : textureLod(sampler2D(shadowMapImage1, nearestClampSampler), uv, 0.0).r;
}

vec2 directionalShadowSize(int slot) {
    return slot == 0 ? vec2(textureSize(sampler2D(shadowMapImage, nearestClampSampler), 0))
                     : vec2(textureSize(sampler2D(shadowMapImage1, nearestClampSampler), 0));
}

// pcf3x3 over a directional slot's map.
float pcf3x3Directional(int slot, vec2 uv, float receiver) {
    vec2 texel = 1.0 / directionalShadowSize(slot);
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float occluder = directionalShadowDepth(slot, uv + vec2(x, y) * texel);
            sum += (receiver <= occluder) ? 1.0 : 0.0;
        }
    }
    return sum / 9.0;
}

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
float getShadowPCSSDirectional(int slot, vec2 uv, float receiverDepth,
                               float orthoRadius, float depthRange) {
    // Clamp so cleared texels (depth 1) are not treated as blockers when the
    // receiver sits outside the tightened cascade depth range.
    float receiverDepthClamped = min(receiverDepth, 0.9999);
    float initialAngle = pcssFractSinRand(gl_FragCoord.xy) * 2.0 * PI;

    // A zero filter count would divide the accumulated visibility by zero and
    // poison the frame with NaN; the renderer always sends 16.
    vec4 pcssParams = directionalShadowPcss(slot);
    int shadowSamples = max(int(pcssParams.x), 1);
    int blockerSamples = int(pcssParams.y);
    float penumbraSize = pcssParams.z;
    float penumbraFalloff = pcssParams.w;

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
            float occluder = directionalShadowDepth(slot, sampleUv);
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
        sum += step(receiverDepthClamped, directionalShadowDepth(slot, sampleUv));
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
        float depthLin = pcssLinearizeDepth(textureLod(tex, sampleUv, 0.0).r, nearClip, farClip);
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
        float depthLin = pcssLinearizeDepth(textureLod(tex, sampleUv, 0.0).r, nearClip, farClip);
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
        float depthLin = pcssCubeStoredToLinear(textureLod(tex, sampleDir, 0.0).r, nearClip, farClip);
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
        float depthLin = pcssCubeStoredToLinear(textureLod(tex, sampleDir, 0.0).r, nearClip, farClip);
        sum += step(receiverDepth, depthLin);
    }
    return sum * invCount;
}

