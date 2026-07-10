#version 450

// One direction of the separable gaussian blur over EVSM moments — mirrors
// metalVsmBlurPass (sigma = filterSize / 3, normalized weights). Runs as a
// fullscreen draw inside the RenderPassVsmBlur render pass; H blurs moments
// into the scratch target, V blurs scratch back into the moments texture.

layout(push_constant) uniform BlurParams {
    vec4 dirInvRes;    // xy = blur direction (1,0)/(0,1), zw = 1 / resolution
    vec4 filterParams; // x = half-kernel size (taps = 2x + 1)
} pc;

layout(set = 0, binding = 0) uniform sampler2D sourceTex;

layout(location = 0) out vec4 outColor;

void main() {
    // gl_FragCoord is framebuffer-space: a 1:1 texel mapping to the
    // equal-resolution source regardless of viewport orientation.
    vec2 uv = gl_FragCoord.xy * pc.dirInvRes.zw;

    int filterSize = int(pc.filterParams.x + 0.5);
    float sigma = max(float(filterSize) / 3.0, 1.0);
    float invSigma2 = 1.0 / (2.0 * sigma * sigma);

    vec4 sum = vec4(0.0);
    float weightSum = 0.0;
    for (int i = -filterSize; i <= filterSize; ++i) {
        float w = exp(-float(i * i) * invSigma2);
        vec2 sampleUv = clamp(uv + pc.dirInvRes.xy * pc.dirInvRes.zw * float(i),
                              vec2(0.0), vec2(1.0));
        sum += texture(sourceTex, sampleUv) * w;
        weightSum += w;
    }
    outColor = sum / weightSum;
}
