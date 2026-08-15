#version 450

// Scalable Ambient Obscurance — port of metalSsaoPass.cpp ssaoFragment
// (spiral-tap SAO from the depth buffer, upstream algorithm).

layout(set = 0, binding = 0) uniform sampler2D depthTex;

layout(set = 0, binding = 4) uniform SsaoParams {
    vec4 p0; // aspect, invRes.xy, randomize
    vec4 p1; // sampleCount, invSampleCount, intensity, power
    vec4 p2; // angleIncCos, angleIncSin, invRadiusSquared, minHorizonAngleSineSquared
    vec4 p3; // bias, peak2, projectionScaleRadius, pad
    vec4 p4; // cameraNear, cameraFar, pad, pad
} pc;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

float getLinearDepth(float rawDepth) {
    float n = pc.p4.x, f = pc.p4.y;
    return (n * f) / (f - rawDepth * (f - n));
}

// Interleaved gradient noise.
float randomIGN(vec2 fragCoord) {
    const vec3 m = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(m.z * fract(dot(fragCoord, m.xy)));
}

vec3 viewPosFromDepth(vec2 uv, float linearDepth, float aspect) {
    return vec3((0.5 - uv) * vec2(aspect, 1.0) * linearDepth, linearDepth);
}

vec3 viewNormal(vec3 position, vec2 uv, vec2 invRes, float aspect) {
    vec2 uvdx = uv + vec2(invRes.x, 0.0);
    vec2 uvdy = uv + vec2(0.0, invRes.y);
    vec3 px = viewPosFromDepth(uvdx, getLinearDepth(texture(depthTex, uvdx).r), aspect);
    vec3 py = viewPosFromDepth(uvdy, getLinearDepth(texture(depthTex, uvdy).r), aspect);
    return normalize(cross(px - position, py - position));
}

void main() {
    float aspect = pc.p0.x;
    vec2 invRes = pc.p0.yz;
    vec2 uv = clamp(gl_FragCoord.xy * invRes, vec2(0.0), vec2(1.0));

    float depth = getLinearDepth(texture(depthTex, uv).r);
    vec3 origin = viewPosFromDepth(uv, depth, aspect);
    // Negated: positive-depth reconstruction flips the cross-product direction
    // relative to upstream's -Z convention (same DEVIATION as the Metal port).
    vec3 normal = -viewNormal(origin, uv, invRes, aspect);

    float occlusion = 0.0;
    float intensity = pc.p1.z;
    if (intensity > 0.0) {
        float noise = randomIGN(gl_FragCoord.xy) + pc.p0.w;
        float angle = (2.0 * PI * 2.4) * noise;
        vec2 tapPos = vec2(cos(angle), sin(angle));
        mat2 angleStep = mat2(pc.p2.x, pc.p2.y, -pc.p2.y, pc.p2.x);

        float ssDiskRadius = pc.p3.z / origin.z;
        float sampleCount = pc.p1.x;
        float invSampleCount = pc.p1.y;

        for (float i = 0.0; i < sampleCount; i += 1.0) {
            float radius = (i + noise + 0.5) * invSampleCount;
            float ssRadius = max(1.0, radius * radius * ssDiskRadius);
            vec2 uvSamplePos = uv + ssRadius * tapPos * invRes;

            float occlusionDepth = getLinearDepth(texture(depthTex, uvSamplePos).r);
            vec3 p = viewPosFromDepth(uvSamplePos, occlusionDepth, aspect);

            vec3 v = p - origin;
            float vv = dot(v, v);
            float vn = dot(v, normal);

            float w = max(0.0, 1.0 - vv * pc.p2.z);
            w = w * w;
            w *= step(vv * pc.p2.w, vn * vn);
            occlusion += w * max(0.0, vn + origin.z * pc.p3.x) / (vv + pc.p3.y);

            tapPos = angleStep * tapPos;
        }
    }

    float ao = max(0.0, 1.0 - occlusion * intensity);
    ao = pow(ao, pc.p1.w);
    outColor = vec4(ao, ao, ao, 1.0);
}
