#version 450

// Compose pass — port of metalComposePass.cpp composeFragment.
// Order: CAS -> SSAO -> DOF (single-pass from depth) -> Bloom -> ToneMap ->
// Vignette -> display gamma. Runs as a fullscreen draw inside the compose
// render pass (usually targeting the swapchain).

layout(set = 0, binding = 0) uniform sampler2D sceneTex;
layout(set = 0, binding = 1) uniform sampler2D bloomTex;
layout(set = 0, binding = 2) uniform sampler2D ssaoTex;
layout(set = 0, binding = 3) uniform sampler2D depthTex;
layout(set = 0, binding = 5) uniform sampler2D colorLut1;
layout(set = 0, binding = 6) uniform sampler2D colorLut2;

layout(set = 0, binding = 4) uniform ComposeParams {
    vec4 p0; // invRes.xy, sharpness, exposure
    vec4 p1; // tonemapMode, ssaoEnabled, bloomEnabled, bloomIntensity
    vec4 p2; // dofEnabled, dofFocusDistance, dofFocusRange, dofBlurRadius
    vec4 p3; // dofCameraNear, dofCameraFar, vignetteEnabled, vignetteInner
    vec4 p4; // vignetteOuter, vignetteCurvature, vignetteIntensity, pad
    vec4 p5; // vignetteColor rgb, pad
    vec4 p6; // fringing, grading enabled, brightness, contrast
    vec4 p7; // saturation, tint rgb
    vec4 p8; // enhance enabled, shadows, highlights, vibrance
    vec4 p9; // dehaze, midtones, lut intensities
    vec4 p10;// lut blend, has lut1, has lut2
} pc;

layout(location = 0) out vec4 outColor;

// ── Tonemapping (same curves as forward.frag / common.metal) ──
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
         / (h * (A * h + B) + (D * F)) - scl * (E / F);
}
vec3 RRTAndODTFit(vec3 v) {
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}
vec3 toneMapAces2(vec3 color) {
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

float maxComp(vec3 c) { return max(c.r, max(c.g, c.b)); }
vec3 toSDR(vec3 c) { return c / (1.0 + maxComp(c)); }
vec3 toHDR(vec3 c) { return c / (1.0 - maxComp(c)); }

// Contrast Adaptive Sharpening.
vec3 applyCas(vec3 color, vec2 uv, float sharpness, vec2 invRes) {
    vec3 a = toSDR(texture(sceneTex, uv + vec2(0.0, -invRes.y)).rgb);
    vec3 b = toSDR(texture(sceneTex, uv + vec2(-invRes.x, 0.0)).rgb);
    vec3 c = toSDR(color);
    vec3 d = toSDR(texture(sceneTex, uv + vec2(invRes.x, 0.0)).rgb);
    vec3 e = toSDR(texture(sceneTex, uv + vec2(0.0, invRes.y)).rgb);

    float min_g = min(a.g, min(b.g, min(c.g, min(d.g, e.g))));
    float max_g = max(a.g, max(b.g, max(c.g, max(d.g, e.g))));
    float sharpening_amount = sqrt(min(1.0 - max_g, min_g) / max(max_g, 1e-6));
    float w = sharpening_amount * sharpness;
    vec3 res = (w * (a + b + d + e) + c) / (4.0 * w + 1.0);
    return toHDR(max(res, vec3(0.0)));
}

vec3 applyVignette(vec3 color, vec2 uv, float inner, float outer,
                   float curvature, float intensity, vec3 vigColor) {
    vec2 curve = pow(abs(uv * 2.0 - 1.0), vec2(1.0 / curvature));
    float edge = pow(length(curve), curvature);
    float vignette = 1.0 - intensity * smoothstep(inner, outer, edge);
    return mix(vigColor, color, vignette);
}

float linearizeSceneDepth(float rawDepth, float cameraNear, float cameraFar) {
    return (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
}

vec3 sampleStripLut(sampler2D lut, vec3 color) {
    color = clamp(color, 0.0, 1.0);
    float blue = color.b * 15.0;
    float slice0 = floor(blue);
    float slice1 = min(slice0 + 1.0, 15.0);
    vec2 uv0 = vec2((slice0 * 16.0 + color.r * 15.0 + 0.5) / 256.0,
                    (color.g * 15.0 + 0.5) / 16.0);
    vec2 uv1 = vec2((slice1 * 16.0 + color.r * 15.0 + 0.5) / 256.0,
                    uv0.y);
    return mix(texture(lut, uv0).rgb, texture(lut, uv1).rgb, fract(blue));
}

// Single-pass DOF from the depth buffer (far blur only, PlayCanvas-style CoC).
vec3 applyDofSinglePass(vec3 sharpColor, vec2 uv, vec2 invRes,
    float focusDistance, float focusRange, float blurRadius,
    float cameraNear, float cameraFar)
{
    float rawDepth = texture(depthTex, uv).r;
    float linearDepth = linearizeSceneDepth(rawDepth, cameraNear, cameraFar);

    float farRange = focusDistance + focusRange * 0.5;
    float invRange = 1.0 / max(focusRange, 0.001);
    float cocFar = clamp((linearDepth - farRange) * invRange, 0.0, 1.0);

    if (cocFar < 0.005) return sharpColor;

    const vec2 offsets[12] = vec2[](
        vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696,  0.457),
        vec2(-0.203,  0.621), vec2( 0.962, -0.195), vec2( 0.473, -0.480),
        vec2( 0.519,  0.767), vec2( 0.185, -0.893), vec2( 0.507,  0.064),
        vec2(-0.321, -0.882), vec2(-0.860,  0.370), vec2( 0.871,  0.414));

    vec2 stepUv = cocFar * blurRadius * invRes;
    vec3 blurSum = vec3(0.0);
    float totalWeight = 0.0;

    for (int i = 0; i < 12; i++) {
        vec2 sampleUV = clamp(uv + offsets[i] * stepUv, vec2(0.0), vec2(1.0));
        float sampleRawDepth = texture(depthTex, sampleUV).r;
        float sampleLinearDepth = linearizeSceneDepth(sampleRawDepth, cameraNear, cameraFar);
        float sampleCoc = clamp((sampleLinearDepth - farRange) * invRange, 0.0, 1.0);
        blurSum += texture(sceneTex, sampleUV).rgb * sampleCoc;
        totalWeight += sampleCoc;
    }

    vec3 blurColor = (totalWeight > 0.0) ? blurSum / totalWeight : sharpColor;
    return mix(sharpColor, blurColor, cocFar);
}

void main() {
    vec2 invRes = pc.p0.xy;
    vec2 uv = gl_FragCoord.xy * invRes;
    vec3 result = texture(sceneTex, uv).rgb;

    // 1. CAS
    if (pc.p0.z > 0.0) {
        result = applyCas(result, uv, pc.p0.z, invRes);
    }

    // 2. SSAO
    if (pc.p1.y > 0.5) {
        float ssao = clamp(texture(ssaoTex, uv).r, 0.0, 1.0);
        result *= ssao;
    }

    // 3. DOF (single-pass from depth)
    if (pc.p2.x > 0.5) {
        result = applyDofSinglePass(result, uv, invRes,
            pc.p2.y, pc.p2.z, pc.p2.w, pc.p3.x, pc.p3.y);
    }

    // 4. Fringing (chromatic aberration). Sits between DOF and bloom, matching upstream
    // compose.js and metalComposePass: red and blue are RE-SAMPLED from the scene
    // texture, so running it after bloom would keep bloom in green only. The offset is
    // the SQUARED distance from centre, as upstream — a linear offset smears the whole
    // mid-field instead of just the corners.
    if (pc.p6.x > 0.0) {
        vec2 centerDistance = uv - 0.5;
        vec2 offset = pc.p6.x * centerDistance * centerDistance;
        result.r = texture(sceneTex, uv - offset).r;
        result.b = texture(sceneTex, uv + offset).b;
    }

    // 5. Bloom
    if (pc.p1.z > 0.5) {
        result += texture(bloomTex, uv).rgb * max(pc.p1.w, 0.0);
    }

    if (pc.p8.x > 0.5) {
        float luma = dot(result, vec3(0.2126, 0.7152, 0.0722));
        float shadowMask = 1.0 - smoothstep(0.0, 0.5, luma);
        float highlightMask = smoothstep(0.5, 1.0, luma);
        float midMask = 1.0 - abs(luma * 2.0 - 1.0);
        result *= exp2(pc.p8.y * shadowMask + pc.p8.z * highlightMask +
                       pc.p9.y * midMask);
        float saturation = maxComp(result) - min(result.r, min(result.g, result.b));
        result = mix(vec3(dot(result, vec3(0.2126, 0.7152, 0.0722))), result,
            1.0 + pc.p8.w * (1.0 - saturation));
        result = max(result - vec3(pc.p9.x * 0.02), 0.0) *
            (1.0 + pc.p9.x);
    }
    if (pc.p6.y > 0.5) {
        result *= pc.p6.z;
        result = (result - 0.5) * pc.p6.w + 0.5;
        float luma = dot(result, vec3(0.2126, 0.7152, 0.0722));
        result = mix(vec3(luma), result, pc.p7.x) * pc.p7.yzw;
    }

    // 6. Tonemapping. Every curve consumes exposure-scaled color (Metal
    // passes exposure into each curve); TONEMAP_NONE applies neither the
    // curve nor exposure — matching metalComposePass exactly.
    result = max(result, vec3(0.0));
    int mode = int(pc.p1.x + 0.5);
    float exposure = pc.p0.w;
    if      (mode == 1) result = toneMapFilmic(result * exposure);
    else if (mode == 2) result = toneMapHejl(result * exposure);
    else if (mode == 3) result = toneMapAcesFit(result * exposure);
    else if (mode == 4) result = toneMapAces2(result * exposure);
    else if (mode == 5) result = toneMapNeutral(result * exposure);
    else if (mode == 6) { /* TONEMAP_NONE: no curve, no exposure */ }
    else                result *= exposure;   // TONEMAP_LINEAR

    if (pc.p10.y > 0.5) {
        result = mix(result, sampleStripLut(colorLut1, result),
            clamp(pc.p9.z, 0.0, 1.0));
    }
    if (pc.p10.z > 0.5) {
        vec3 graded2 = mix(result, sampleStripLut(colorLut2, result),
            clamp(pc.p9.w, 0.0, 1.0));
        result = mix(result, graded2, clamp(pc.p10.x, 0.0, 1.0));
    }

    // 7. Vignette (tonemapped linear space, before gamma)
    if (pc.p3.z > 0.5) {
        result = applyVignette(result, uv, pc.p3.w, pc.p4.x, pc.p4.y, pc.p4.z, pc.p5.rgb);
    }

    // 8. Display gamma (linear UNORM swapchain, matching Metal BGRA8Unorm)
    result = pow(max(result, vec3(0.0)) + 0.0000001, vec3(1.0 / 2.2));
    outColor = vec4(result, 1.0);
}
