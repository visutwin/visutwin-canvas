#version 450

// TAA resolve — port of metalTaaPass.cpp taaFragment: depth reprojection into
// the previous frame, Catmull-Rom or bilinear history fetch, neighborhood
// color clamping, 5% history blend (100% on offscreen reprojection).

layout(set = 0, binding = 0) uniform sampler2D sourceTex;
layout(set = 0, binding = 1) uniform sampler2D historyTex;
layout(set = 0, binding = 2) uniform sampler2D depthTex;

layout(set = 0, binding = 4) uniform TaaParams {
    mat4 viewProjectionPrevious;
    mat4 viewProjectionInverse;
    vec4 jitters;        // xy = current-frame NDC jitter
    vec4 texSizeFlags;   // xy = texture size, z = highQuality, w = historyValid
    vec4 cameraParams;
} pc;

layout(location = 0) out vec4 outColor;

vec2 reproject(vec2 uv, float depth) {
    // Depth buffer stores (ndcZ_gl + 1) / 2 — undo to OpenGL NDC Z.
    depth = depth * 2.0 - 1.0;

    // UV is top-left-origin (Metal convention, matched by our viewport flip);
    // projection matrices use GL NDC (+Y up): ndcY = 1 - 2*uv.y.
    vec4 ndc = vec4(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y, depth, 1.0);
    ndc.xy -= pc.jitters.xy;

    vec4 worldPosition = pc.viewProjectionInverse * ndc;
    worldPosition /= worldPosition.w;

    vec4 screenPrevious = pc.viewProjectionPrevious * worldPosition;
    vec2 prevNdc = screenPrevious.xy / screenPrevious.w;
    return vec2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
}

vec4 sampleCatmullRom(vec2 uv, vec2 texSize) {
    vec2 samplePos = uv * texSize;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - texPos1;

    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);

    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / (w1 + w2);

    vec2 texPos0 = (texPos1 - 1.0) / texSize;
    vec2 texPos3 = (texPos1 + 2.0) / texSize;
    vec2 texPos12 = (texPos1 + offset12) / texSize;

    vec4 result = vec4(0.0);
    result += textureLod(historyTex, vec2(texPos0.x, texPos0.y), 0.0) * w0.x * w0.y;
    result += textureLod(historyTex, vec2(texPos12.x, texPos0.y), 0.0) * w12.x * w0.y;
    result += textureLod(historyTex, vec2(texPos3.x, texPos0.y), 0.0) * w3.x * w0.y;

    result += textureLod(historyTex, vec2(texPos0.x, texPos12.y), 0.0) * w0.x * w12.y;
    result += textureLod(historyTex, vec2(texPos12.x, texPos12.y), 0.0) * w12.x * w12.y;
    result += textureLod(historyTex, vec2(texPos3.x, texPos12.y), 0.0) * w3.x * w12.y;

    result += textureLod(historyTex, vec2(texPos0.x, texPos3.y), 0.0) * w0.x * w3.y;
    result += textureLod(historyTex, vec2(texPos12.x, texPos3.y), 0.0) * w12.x * w3.y;
    result += textureLod(historyTex, vec2(texPos3.x, texPos3.y), 0.0) * w3.x * w3.y;
    return result;
}

vec4 colorClamp(vec2 uv, vec4 historyColor, vec2 texSize) {
    vec3 minColor = vec3(9999.0);
    vec3 maxColor = vec3(-9999.0);
    for (float x = -1.0; x <= 1.0; x += 1.0) {
        for (float y = -1.0; y <= 1.0; y += 1.0) {
            vec3 color = texture(sourceTex, uv + vec2(x, y) / texSize).rgb;
            minColor = min(minColor, color);
            maxColor = max(maxColor, color);
        }
    }
    return vec4(clamp(historyColor.rgb, minColor, maxColor), historyColor.a);
}

void main() {
    vec2 texSize = pc.texSizeFlags.xy;
    vec2 uv = clamp(gl_FragCoord.xy / texSize, vec2(0.0), vec2(1.0));

    vec4 srcColor = texture(sourceTex, uv);

    if (pc.texSizeFlags.w < 0.5) {   // no valid history yet
        outColor = srcColor;
        return;
    }

    float depth = texture(depthTex, uv).r;
    vec2 historyUv = reproject(uv, depth);

    vec4 historyColor = (pc.texSizeFlags.z > 0.5)
        ? sampleCatmullRom(historyUv, texSize)
        : texture(historyTex, historyUv);

    vec4 historyColorClamped = colorClamp(uv, historyColor, texSize);

    float mixFactor = (historyUv.x < 0.0 || historyUv.x > 1.0 ||
                       historyUv.y < 0.0 || historyUv.y > 1.0) ? 1.0 : 0.05;

    outColor = mix(historyColorClamped, srcColor, mixFactor);
}
