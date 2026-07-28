#version 450

#include "shader_features.glsl"

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragWorldNormal;
layout(location = 2) in vec2 fragUV0;
layout(location = 3) in vec2 fragUV1;
layout(location = 4) in vec4 fragWorldTangent;
layout(location = 5) in float fragViewDepth;
layout(location = 6) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

// Set 0 (dynamic UBO): per-draw material. Declares the prefix of the engine's
// MaterialUniforms struct that this shader consumes — std140 offsets match the
// C++ MaterialUniforms layout exactly.
layout(set = 0, binding = 0) uniform MaterialData {
    vec4 baseColor;
    vec4 emissiveColor;
    uint flags;
    uint occludeSpecularMode;
    float alphaCutoff;
    float metallicFactor;
    float roughnessFactor;
    float normalScale;
    float occlusionStrength;
    float occludeSpecularIntensity;
    vec4 baseColorTransform0;
    vec4 baseColorTransform1;
    vec4 normalTransform0;
    vec4 normalTransform1;
    vec4 metalRoughTransform0;
    vec4 metalRoughTransform1;
    vec4 occlusionTransform0;
    vec4 occlusionTransform1;
    vec4 emissiveTransform0;
    vec4 emissiveTransform1;
    float clearCoatFactor;
    float clearCoatRoughness;
    float clearCoatBumpiness;
    float heightMapFactor;
    float anisotropy;
    float transmissionFactor;
    float refractionIndex;
    float thickness;
    vec4 sheenColor;
    vec4 iridescenceParams;
    vec4 specGlossParams;
    vec4 detailDisplacementParams;
    vec4 detailNormalTransform0;
    vec4 detailNormalTransform1;
    vec4 attenuationParams;
    vec4 dispersionParams;
} material;

// Set 1: material texture slots (engine slot numbering).
layout(set = 1, binding = 0) uniform sampler2D baseColorMap;     // 0
layout(set = 1, binding = 1) uniform sampler2D normalMap;        // 1
layout(set = 1, binding = 3) uniform sampler2D metalRoughMap;    // 3 (glTF: G=rough, B=metal)
layout(set = 1, binding = 4) uniform sampler2D occlusionMap;     // 4 (R = AO)
layout(set = 1, binding = 5) uniform sampler2D emissiveMap;      // 5
layout(set = 1, binding = 19) uniform sampler2D lightMap;        // 19

// Set 2 (dynamic UBO): per-pass lighting. Matches VulkanLightingUBO.
struct Light {
    vec4 positionRange;   // xyz position, w range
    vec4 directionType;   // xyz direction, w type (0=dir, 1=point, 2=spot)
    vec4 colorIntensity;  // rgb color, w intensity
    vec4 coneParams;      // innerCos, outerCos, falloffLinear, localShadowIndex(-1/0/1)
    vec4 areaRightHalfWidth;
    vec4 areaUpHalfHeight;
};

layout(set = 2, binding = 0) uniform LightingData {
    vec4 ambient;             // rgb ambient
    vec4 cameraPosExposure;   // xyz camera position, w exposure
    uvec4 lightCount;         // x = active light count
    Light lights[8];
    vec4 fogColorDensity;     // rgb fog color, w density
    vec4 fogStartEndType;     // start, end, type (0=off,1=linear,2=exp), pad
    vec4 envParams;           // skyboxIntensity, hasEnvAtlas, encoding, skyboxMip
    mat4 shadowMatrices[4];   // per-cascade world → shadow-atlas UV + depth
    vec4 shadowCascadeDistances; // per-cascade far split (view-space depth)
    vec4 shadowParams;        // enabled, numCascades, depthBias, strength
    vec4 shadowParams2;       // normalBias, cascadeBlend, toneMapping, enableNormalMaps
    vec4 pcssParams;          // filterSamples, blockerSamples, penumbraSize, penumbraFalloff
    vec4 pcssCascadeRadii;       // per-cascade shadow-camera ortho half-extent
    vec4 pcssCascadeDepthRanges; // per-cascade caster depth span (far - near)
    mat4 localShadowMatrix0;  // spot slot 0: world → shadow UV + depth
    mat4 localShadowMatrix1;  // spot slot 1
    vec4 localShadowParams0;  // depthBias, normalBias, intensity, isOmni
    vec4 localShadowParams1;
    vec4 omniShadowParams0;   // near, far, depthBias, intensity
    vec4 omniShadowParams1;
    vec4 localShadowPcss0;    // searchArea UV (0 = off), near, far, pad
    vec4 localShadowPcss1;
    vec4 skyParams2;          // xyz sky dome center, w flags: bit0 cubemap, bit1 dome
    vec4 ambientSH[9];
    vec4 clusterBoundsMin;
    vec4 clusterBoundsRange;
    vec4 clusterCellsCountByBoundsSize;
    uvec4 clusterParams;
    uvec4 clusterParams2;
    vec4 reflectionProbeBoxMin;
    vec4 reflectionProbeBoxMax;
    vec4 reflectionProbePosition;
    vec4 reflectionProbeParams;
} lighting;

struct ClusterLight {
    vec4 positionRange;
    vec4 directionSpot;
    vec4 colorIntensity;
    vec4 params;
    mat4 shadowMatrix;
    vec4 shadowData;
};
layout(std430, set = 5, binding = 0) readonly buffer ClusterLights {
    ClusterLight values[];
} clusterLights;
layout(std430, set = 5, binding = 1) readonly buffer ClusterCells {
    uint values[];
} clusterCells;

// Set 3: scene textures. Binding 0 = equirectangular environment atlas
// (IBL irradiance + roughness mips + skybox source). Binding 1 = directional
// cascaded shadow-map depth atlas. Bindings 2-3 = local spot-light 2D depth
// maps; bindings 4-5 = omni point-light cubemap depth maps. All shadow maps
// are sampled through a NEAREST clamp sampler with manual depth comparison.
layout(set = 3, binding = 0) uniform sampler2D envAtlas;
layout(set = 3, binding = 1) uniform sampler2D shadowMap;
layout(set = 3, binding = 2) uniform sampler2D localShadowMap0;
layout(set = 3, binding = 3) uniform sampler2D localShadowMap1;
layout(set = 3, binding = 4) uniform samplerCube omniShadowCube0;
layout(set = 3, binding = 5) uniform samplerCube omniShadowCube1;
layout(set = 3, binding = 6) uniform samplerCube skyboxCube;
layout(set = 3, binding = 7) uniform samplerCube reflectionProbeCube;

const float PI = 3.14159265359;

// 3×3 percentage-closer filter: average binary depth comparisons over the
// texel neighbourhood.  `receiver` is the (biased) light-space depth of the
// shaded point; a texel is lit when its stored occluder depth is no nearer.
float pcf3x3(sampler2D tex, vec2 uv, float receiver) {
    vec2 texel = 1.0 / vec2(textureSize(tex, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float occluder = texture(tex, uv + vec2(x, y) * texel).r;
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
    float visible;
    if (lighting.shadowParams.x > 1.5) {
        visible = sampleShadowVSM16(coord.xy, coord.z, max(lighting.shadowParams.z, 1e-4));
    } else if (vtFeatureEnabled(VT_FEATURE_PCSS_SHADOWS_BIT)) {
        visible = getShadowPCSSDirectional(coord.xy,
            coord.z - lighting.shadowParams.z,
            lighting.pcssCascadeRadii[cascade],
            lighting.pcssCascadeDepthRanges[cascade]);
    } else {
        float receiver = coord.z - lighting.shadowParams.z;
        visible = pcf3x3(shadowMap, coord.xy, receiver);
    }
    return mix(1.0, visible, lighting.shadowParams.w);
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
        float denom = (farV - nearV) * d;
        float compareValue = farV * (d - nearV) / max(denom, 1e-6) - bias;

        float occluder = (slot == 0) ? texture(omniShadowCube0, dir).r
                                     : texture(omniShadowCube1, dir).r;
        visible = (compareValue <= occluder) ? 1.0 : 0.0;
    }
    return mix(1.0 - clamp(intensity, 0.0, 1.0), 1.0, visible);
}

// ── Environment atlas layout (matches engine bake / Metal common.metal) ──
const float ATLAS_SIZE = 512.0;
const float ATLAS_SEAM = 1.0 / 512.0;

// Equirectangular direction → atlas UV (atan2 azimuth, asin elevation).
vec2 dirToEquirect(vec3 dir) {
    vec2 sph = vec2(atan(dir.x, dir.z), asin(clamp(dir.y, -1.0, 1.0)));
    vec2 uv = sph / vec2(6.28318530718, 3.14159265359) + 0.5;
    return vec2(uv.x, 1.0 - uv.y);
}

// Map a [0,1] uv into a packed sub-rect (x,y,w,h), insetting by the 1px seam.
vec2 mapRect(vec2 uv, vec4 rect) {
    return vec2(mix(rect.x + ATLAS_SEAM, rect.x + rect.z - ATLAS_SEAM, uv.x),
                mix(rect.y + ATLAS_SEAM, rect.y + rect.w - ATLAS_SEAM, uv.y));
}

// Lambert irradiance sub-rect: 64×32 region at (128,384).
vec2 mapAmbientUv(vec2 uv) {
    return mapRect(uv, vec4(128.0 / ATLAS_SIZE, 384.0 / ATLAS_SIZE,
                            64.0 / ATLAS_SIZE, 32.0 / ATLAS_SIZE));
}

// Prefiltered roughness mip `level` down the left edge: rect (0, 1-t, t, t/2).
vec2 mapRoughnessUv(vec2 uv, float level) {
    float t = 1.0 / exp2(level);
    return mapRect(uv, vec4(0.0, 1.0 - t, t, t * 0.5));
}

vec3 decodeRGBP(vec4 raw) { vec3 c = raw.rgb * (-raw.a * 7.0 + 8.0); return c * c; }
vec3 decodeRGBM(vec4 raw) { vec3 c = (8.0 * raw.a) * raw.rgb; return c * c; }
vec3 srgbToLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }

vec3 decodeEnv(vec4 raw) {
    uint enc = uint(lighting.envParams.z + 0.5);
    if (enc == 1u) return decodeRGBP(raw);
    if (enc == 2u) return decodeRGBM(raw);
    return srgbToLinear(raw.rgb);
}

// ── Tonemapping (parity with the Metal common.metal toneMap dispatch) ──
// Modes match scene/constants.h: 0=linear, 1=filmic, 2=hejl, 3=aces,
// 4=aces2, 5=neutral, 6=none. Applied before display-gamma encode.

vec3 toneMapAcesFit(vec3 x) {
    const float tA = 2.51, tB = 0.03, tC = 2.43, tD = 0.59, tE = 0.14;
    return (x * (tA * x + tB)) / (x * (tC * x + tD) + tE);
}

vec3 uncharted2Tonemap(vec3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 toneMapFilmic(vec3 color) {
    const float W = 11.2;
    color = uncharted2Tonemap(color * 2.0);
    return color * (1.0 / uncharted2Tonemap(vec3(W)));
}

vec3 toneMapHejl(vec3 color) {
    const float A = 0.22, B = 0.3, C = 0.1, D = 0.2, E = 0.01, F = 0.3;
    const float scl = 1.25;
    vec3 h = max(vec3(0.0), color - 0.004);
    return (h * ((scl * A) * h + scl * (C * B)) + scl * (D * E))
         / (h * (A * h + B) + (D * F))
         - scl * (E / F);
}

vec3 RRTAndODTFit(vec3 v) {
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

vec3 toneMapAces2(vec3 color) {
    // GLSL mat3 constructors take COLUMNS; these are the transposes of the
    // row-major matrices in common.metal (which multiplies vector * matrix).
    const mat3 ACESInputMat = mat3(
        0.59719, 0.07600, 0.02840,
        0.35458, 0.90834, 0.13383,
        0.04823, 0.01566, 0.83777);
    const mat3 ACESOutputMat = mat3(
         1.60475, -0.10208, -0.00327,
        -0.53108,  1.10813, -0.07276,
        -0.07367, -0.00605,  1.07602);
    color /= 0.6;
    color = ACESInputMat * color;
    color = RRTAndODTFit(color);
    color = ACESOutputMat * color;
    return clamp(color, vec3(0.0), vec3(1.0));
}

vec3 toneMapNeutral(vec3 color) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;
    float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, vec3(newPeak), g);
}

// Exposure-scaled color -> tonemapped color, by scene mode.
vec3 applyToneMap(vec3 color) {
    int mode = int(lighting.shadowParams2.z + 0.5);
    if (mode == 1) return toneMapFilmic(color);
    if (mode == 2) return toneMapHejl(color);
    if (mode == 3) return toneMapAcesFit(color);
    if (mode == 4) return toneMapAces2(color);
    if (mode == 5) return toneMapNeutral(color);
    return color; // LINEAR (0) and NONE (6): exposure only
}

// Material flag bits (subset of the engine MaterialUniforms flags).
const uint FLAG_ALPHA_TEST   = 1u << 1;
const uint FLAG_HAS_NORMAL   = 1u << 2;
const uint FLAG_DOUBLE_SIDED = 1u << 3;
const uint FLAG_BASE_UV1     = 1u << 4;   // per-map UV-set selection (uv1 when set)
const uint FLAG_NORMAL_UV1   = 1u << 5;
const uint FLAG_HAS_METALROUGH = 1u << 6;
const uint FLAG_METALROUGH_UV1 = 1u << 7;
const uint FLAG_SKYBOX         = 1u << 8;
const uint FLAG_HAS_OCCLUSION  = 1u << 9;
const uint FLAG_OCCLUSION_UV1  = 1u << 10;
const uint FLAG_HAS_EMISSIVE   = 1u << 11;
const uint FLAG_EMISSIVE_UV1   = 1u << 12;

vec2 applyUvTransform(vec2 uv, vec4 row0, vec4 row1) {
    vec3 h = vec3(uv, 1.0);
    return vec2(dot(h, row0.xyz), dot(h, row1.xyz));
}

// Distance attenuation: inverse-square with a smooth range window, or linear
// falloff when coneParams.z != 0 (matches the engine's falloffModeLinear).
float distanceAttenuation(float dist, float range, float linearFalloff) {
    if (range <= 0.0) {
        return 1.0;
    }
    float t = clamp(dist / range, 0.0, 1.0);
    if (linearFalloff > 0.5) {
        return clamp(1.0 - t, 0.0, 1.0);
    }
    float invSq = 1.0 / max(dist * dist, 1e-4);
    float window = clamp(1.0 - t * t * t * t, 0.0, 1.0);
    return invSq * window * window;
}

// GGX normal distribution.
float distributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

// Smith geometry term (Schlick-GGX, direct lighting k).
float geometrySmith(float NdotV, float NdotL, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float gv = NdotV / (NdotV * (1.0 - k) + k);
    float gl = NdotL / (NdotL * (1.0 - k) + k);
    return gv * gl;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

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
        if ((skyFlags & 1u) != 0u) {
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

    vec4 baseSample = vtFeatureEnabled(VT_FEATURE_BASE_COLOR_MAP_BIT)
        ? texture(baseColorMap, uvBase) : vec4(1.0);
    // fragColor is vec4(1) except for the vertex-color / point-cloud vertex
    // variants, which feed the mesh's per-vertex color through.
    vec4 albedo = material.baseColor * baseSample * fragColor;

    if (vtFeatureEnabled(VT_FEATURE_ALPHA_TEST_BIT) &&
        albedo.a < material.alphaCutoff) {
        discard;
    }

    // Point-cloud (unlit) path: the point vertex variant writes a zero world
    // normal as its sentinel — no surface lighting, just exposure + tonemap +
    // gamma on the tinted color (mirrors Metal's unlit point shader).
    if (vtFeatureEnabled(VT_FEATURE_UNLIT_BIT) ||
        dot(fragWorldNormal, fragWorldNormal) < 1e-6) {
        vec3 unlit = albedo.rgb * lighting.cameraPosExposure.w;
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
    if (vtFeatureEnabled(VT_FEATURE_NORMAL_MAP_BIT) &&
        lighting.shadowParams2.w > 0.5) {
        vec3 T = normalize(fragWorldTangent.xyz);
        // Re-orthonormalize (Gram-Schmidt) and build the bitangent with the
        // handedness sign carried in tangent.w.
        T = normalize(T - N * dot(N, T));
        vec3 B = cross(N, T) * fragWorldTangent.w;
        vec3 tn = texture(normalMap, uvNormal).xyz * 2.0 - 1.0;
        tn.xy *= material.normalScale;
        N = normalize(mat3(T, B, N) * tn);
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

    vec3 color = vec3(0.0);

    uint count = min(lighting.lightCount.x, 8u);
    for (uint i = 0u; i < count; ++i) {
        Light light = lighting.lights[i];
        uint type = uint(light.directionType.w + 0.5);

        vec3 L;
        float atten = 1.0;
        if (type == 0u) {
            L = normalize(-light.directionType.xyz);
            // Directional light is the CSM shadow caster.
            atten = sampleDirectionalShadow(fragWorldPos, fragViewDepth, N, L);
        } else {
            vec3 lightPosition = light.positionRange.xyz;
            if (type == 3u) {
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
            if (type == 2u) {
                float cd = dot(normalize(-light.directionType.xyz), L);
                float spot = clamp((cd - light.coneParams.y) /
                                   max(light.coneParams.x - light.coneParams.y, 1e-4), 0.0, 1.0);
                atten *= spot * spot;
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
        float G = geometrySmith(NdotV, NdotL, roughness);
        vec3 F = fresnelSchlick(VdotH, F0);

        vec3 specular = (D * G) * F / max(4.0 * NdotV * NdotL, 1e-4);
        vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

        vec3 radiance = light.colorIntensity.rgb * light.colorIntensity.w * atten;
        color += (kD * diffuseAlbedo / PI + specular) * radiance * NdotL;
        if (vtFeatureEnabled(VT_FEATURE_CLEARCOAT_BIT)) {
            float ccRough = clamp(material.clearCoatRoughness, 0.04, 1.0);
            float ccD = distributionGGX(NdotH, ccRough);
            float ccG = geometrySmith(NdotV, NdotL, ccRough);
            vec3 ccF = fresnelSchlick(VdotH, vec3(0.04));
            color += material.clearCoatFactor * ccD * ccG * ccF /
                max(4.0 * NdotV, 1e-4) * radiance;
        }
        if (vtFeatureEnabled(VT_FEATURE_SHEEN_BIT)) {
            float velvet = pow(1.0 - max(NdotH, 0.0),
                mix(2.0, 8.0, material.sheenColor.w));
            color += material.sheenColor.rgb * velvet * radiance * NdotL;
        }
    }

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
                    atten *= clamp((cone - cl.directionSpot.w) /
                        max(cl.params.x - cl.directionSpot.w, 1e-4), 0.0, 1.0);
                }
                float nl = max(dot(N, L), 0.0);
                vec3 H = normalize(L + V);
                float nh = max(dot(N, H), 0.0);
                float vh = max(dot(V, H), 0.0);
                float D = distributionGGX(nh, roughness);
                float G = geometrySmith(NdotV, nl, roughness);
                vec3 F = fresnelSchlick(vh, F0);
                vec3 kd = (1.0 - F) * (1.0 - metallic);
                vec3 radiance = cl.colorIntensity.rgb *
                    cl.colorIntensity.w * atten;
                color += (kd * diffuseAlbedo / PI +
                    D * G * F / max(4.0 * NdotV * nl, 1e-4)) *
                    radiance * nl;
            }
        }
    }

    // Indirect lighting.  With an environment atlas: image-based diffuse
    // irradiance + roughness-prefiltered specular reflection.  Without one:
    // a flat ambient term plus a Fresnel-weighted specular floor so metals
    // aren't pitch black.
    vec3 indirect;
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
    } else {
        indirect = lighting.ambient.rgb * diffuseAlbedo + lighting.ambient.rgb * F0;
    }
    if (vtFeatureEnabled(VT_FEATURE_LIGHTMAP_BIT)) {
        indirect += srgbToLinear(texture(lightMap, fragUV1).rgb) * diffuseAlbedo;
    }
    color += indirect * ao;
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
        color += textureLod(reflectionProbeCube, probeDirection,
            roughness * lighting.reflectionProbeParams.z).rgb *
            F0 * lighting.reflectionProbeParams.y;
    }
    if (vtFeatureEnabled(VT_FEATURE_TRANSMISSION_BIT)) {
        float transmission = clamp(material.transmissionFactor, 0.0, 1.0);
        vec3 transmitted = indirect;
        if (material.attenuationParams.w > 0.0) {
            transmitted *= pow(max(material.attenuationParams.rgb, vec3(1e-4)),
                vec3(material.thickness / material.attenuationParams.w));
        }
        color = mix(color, transmitted, transmission);
    }

    // Emissive.
    vec3 emissive = material.emissiveColor.rgb;
    if (vtFeatureEnabled(VT_FEATURE_EMISSIVE_MAP_BIT)) {
        emissive *= texture(emissiveMap, uvEmissive).rgb;
    }
    color += emissive;

    // Fog (linear or exponential) toward the fog color.
    float fogType = vtFeatureEnabled(VT_FEATURE_FOG_BIT)
        ? lighting.fogStartEndType.z : 0.0;
    if (fogType > 0.5) {
        float dist = length(lighting.cameraPosExposure.xyz - fragWorldPos);
        float f;
        if (fogType < 1.5) {
            f = clamp((lighting.fogStartEndType.y - dist) /
                      max(lighting.fogStartEndType.y - lighting.fogStartEndType.x, 1e-4), 0.0, 1.0);
        } else {
            float d = dist * lighting.fogColorDensity.w;
            f = clamp(exp(-d * d), 0.0, 1.0);
        }
        color = mix(lighting.fogColorDensity.rgb, color, f);
    }

    // Exposure, tonemap, then display-gamma encode (swapchain is a linear
    // UNORM target, matching the Metal BGRA8Unorm drawable).
    color *= lighting.cameraPosExposure.w;
    color = applyToneMap(color);
    color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));

    outColor = vec4(color, albedo.a);
}
