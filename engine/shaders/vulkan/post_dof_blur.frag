#version 450
layout(location=0) out vec4 outColor;
layout(set=0,binding=0) uniform sampler2D nearTexture;
layout(set=0,binding=1) uniform sampler2D farTexture;
layout(set=0,binding=2) uniform sampler2D cocTexture;
layout(set=0,binding=3) uniform sampler2D unused3;
layout(std140,set=0,binding=4) uniform Params {
    vec4 radiiInvRes; // near radius, far radius, invW, invH
    vec4 rings;       // ring count, points per ring
} p;
void main() {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(farTexture, 0));
    vec2 centerCoc = texture(cocTexture, uv).rg;
    vec3 nearSum = texture(nearTexture, uv).rgb;
    vec3 farSum = texture(farTexture, uv).rgb;
    float nearWeight = 1.0;
    float farWeight = 1.0;
    int ringCount = clamp(int(p.rings.x), 1, 8);
    int points = clamp(int(p.rings.y), 3, 16);
    for (int ring = 1; ring <= ringCount; ++ring) {
        int samples = points * ring;
        for (int i = 0; i < samples; ++i) {
            float angle = 6.28318530718 * float(i) / float(samples);
            vec2 direction = vec2(cos(angle), sin(angle));
            float radius = float(ring) / float(ringCount);
            vec2 nearUv = uv + direction * radius * p.radiiInvRes.x *
                p.radiiInvRes.zw;
            vec2 farUv = uv + direction * radius * p.radiiInvRes.y *
                p.radiiInvRes.zw;
            float nw = texture(cocTexture, nearUv).r;
            float fw = texture(cocTexture, farUv).g;
            nearSum += texture(nearTexture, nearUv).rgb * nw;
            farSum += texture(farTexture, farUv).rgb * fw;
            nearWeight += nw;
            farWeight += fw;
        }
    }
    vec3 nearColor = nearSum / nearWeight;
    vec3 farColor = farSum / farWeight;
    vec3 result = mix(farColor, nearColor, centerCoc.r);
    outColor = vec4(result, max(centerCoc.r, centerCoc.g));
}
