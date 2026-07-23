#version 450

// Vertex-color variant: the 72-byte interleaved layout appends a vec4 color
// after uv1 (attribute location 5, offset 56). Identical to forward_basic.vert
// otherwise; the color reaches the fragment stage at location 6 and is
// multiplied into albedo. Selected when the vertex format declares COLOR.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV0;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec2 inUV1;
layout(location = 5) in vec4 inColor;

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

void main() {
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    gl_Position = pc.viewProjection * worldPos;

    fragViewDepth = gl_Position.w;
    fragWorldPos = worldPos.xyz;

    mat3 normalMatrix = mat3(pc.model);
    fragWorldNormal = normalize(normalMatrix * inNormal);
    fragWorldTangent = vec4(normalize(normalMatrix * inTangent.xyz), inTangent.w);
    fragUV0 = inUV0;
    fragUV1 = inUV1;
    fragColor = inColor;
}
