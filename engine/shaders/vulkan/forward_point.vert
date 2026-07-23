#version 450

// Point-cloud variant: the 28-byte layout packs position (vec3 @0) and a vec4
// color. Selected for point primitives whose format declares COLOR. Writes a
// ZERO world normal as the unlit sentinel — the fragment stage detects it and
// outputs the tinted color without surface lighting (mirrors Metal, where
// point clouds use the unlit shader path).

layout(location = 0) in vec3 inPosition;
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
    gl_PointSize = 1.0;

    fragViewDepth = gl_Position.w;
    fragWorldPos = worldPos.xyz;
    fragWorldNormal = vec3(0.0);          // unlit sentinel
    fragWorldTangent = vec4(1.0, 0.0, 0.0, 1.0);
    fragUV0 = vec2(0.0);
    fragUV1 = vec2(0.0);
    fragColor = inColor;
}
