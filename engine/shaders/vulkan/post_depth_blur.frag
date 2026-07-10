#version 450

// Depth-aware (bilateral) separable blur — port of metalDepthAwareBlurPass.
// One direction per pass (direction in params); used to smooth the SSAO term.

layout(set = 0, binding = 0) uniform sampler2D sourceTex;
layout(set = 0, binding = 1) uniform sampler2D depthTex;

layout(set = 0, binding = 4) uniform BlurParams {
    vec4 p0; // invRes.xy, dir.xy
    vec4 p1; // filterSize, cameraNear, cameraFar, pad
} pc;

layout(location = 0) out vec4 outColor;

float getLinearDepth(float rawDepth) {
    float n = pc.p1.y, f = pc.p1.z;
    return (n * f) / (f - rawDepth * (f - n));
}

float bilateralWeight(float depth, float sampleDepth) {
    float diff = sampleDepth - depth;
    return max(0.0, 1.0 - diff * diff);
}

void main() {
    vec2 invRes = pc.p0.xy;
    vec2 dir = pc.p0.zw;
    vec2 uv = clamp(gl_FragCoord.xy * invRes, vec2(0.0), vec2(1.0));

    // Center pixel pre-seeded with weight 1 (it trivially passes the
    // bilateral test); the loop skips i == 0 to avoid double-counting.
    float depth = getLinearDepth(texture(depthTex, uv).r);
    float totalWeight = 1.0;
    float sum = texture(sourceTex, uv).r;

    int filterSize = int(pc.p1.x + 0.5);
    float sigma = max(float(filterSize) / 3.0, 1.0);
    float invSigma2 = 1.0 / (2.0 * sigma * sigma);

    for (int i = -filterSize; i <= filterSize; ++i) {
        if (i == 0) { continue; }
        float w = exp(-float(i * i) * invSigma2);
        vec2 sampleUv = clamp(uv + dir * invRes * float(i), vec2(0.0), vec2(1.0));

        float sampleColor = texture(sourceTex, sampleUv).r;
        float sampleDepth = getLinearDepth(texture(depthTex, sampleUv).r);

        float bilateral = bilateralWeight(depth, sampleDepth) * w;
        sum += sampleColor * bilateral;
        totalWeight += bilateral;
    }

    float ao = sum / totalWeight;
    outColor = vec4(ao, 0.0, 0.0, 1.0);
}
