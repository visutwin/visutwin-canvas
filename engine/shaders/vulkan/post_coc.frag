#version 450
layout(location=0) out vec4 outColor;
layout(set=0,binding=0) uniform sampler2D depthTexture;
layout(set=0,binding=1) uniform sampler2D unused1;
layout(set=0,binding=2) uniform sampler2D unused2;
layout(set=0,binding=3) uniform sampler2D unused3;
layout(std140,set=0,binding=4) uniform Params {
    vec4 focus; // distance, range, near, far
    vec4 flags; // nearBlur
} p;
float linearDepth(float d) {
    return p.focus.z * p.focus.w /
        max(p.focus.w - d * (p.focus.w - p.focus.z), 1e-6);
}
void main() {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(depthTexture, 0));
    float z = linearDepth(texture(depthTexture, uv).r);
    float halfRange = max(p.focus.y * 0.5, 1e-5);
    float farCoc = clamp((z - (p.focus.x + halfRange)) / halfRange, 0.0, 1.0);
    float nearCoc = p.flags.x > 0.5
        ? clamp(((p.focus.x - halfRange) - z) / halfRange, 0.0, 1.0) : 0.0;
    outColor = vec4(nearCoc, farCoc, 0.0, 1.0);
}
