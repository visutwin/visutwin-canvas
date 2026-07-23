#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV0;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec2 inUV1;

layout(std430, set = 4, binding = 1) readonly buffer MorphDeltaData {
    vec4 deltas[];
} morph;
layout(std140, set = 4, binding = 2) uniform MorphParams {
    uvec4 counts;
    uvec4 indices0;
    uvec4 indices1;
    vec4 weights0;
    vec4 weights1;
} morphParams;

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    mat4 model;
} pc;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragWorldNormal;
layout(location = 2) out vec2 fragUV0;
layout(location = 3) out vec2 fragUV1;
layout(location = 4) out vec4 fragWorldTangent;
layout(location = 5) out float fragViewDepth;
layout(location = 6) out vec4 fragColor;

void applyMorph(inout vec3 position, inout vec3 normal) {
    for (uint k = 0u; k < morphParams.counts.x; ++k) {
        uint target = k < 4u
            ? morphParams.indices0[k] : morphParams.indices1[k - 4u];
        float weight = k < 4u
            ? morphParams.weights0[k] : morphParams.weights1[k - 4u];
        uint base = (target * morphParams.counts.y + uint(gl_VertexIndex)) * 2u;
        position += weight * morph.deltas[base].xyz;
        normal += weight * morph.deltas[base + 1u].xyz;
    }
}

void main() {
    vec3 position = inPosition;
    vec3 normal = inNormal;
    applyMorph(position, normal);
    vec4 worldPos = pc.model * vec4(position, 1.0);
    gl_Position = pc.viewProjection * worldPos;
    mat3 normalMatrix = mat3(pc.model);
    fragWorldPos = worldPos.xyz;
    fragWorldNormal = normalize(normalMatrix * normal);
    fragWorldTangent =
        vec4(normalize(normalMatrix * inTangent.xyz), inTangent.w);
    fragUV0 = inUV0;
    fragUV1 = inUV1;
    fragViewDepth = gl_Position.w;
    fragColor = vec4(1.0);
}
