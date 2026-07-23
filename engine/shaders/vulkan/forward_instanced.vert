#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV0;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec2 inUV1;

// Per-instance data (binding 1, VK_VERTEX_INPUT_RATE_INSTANCE):
// column-major model matrix as 4 vec4 columns.  The 16-byte per-instance
// RGBA color that follows in the buffer is not consumed yet — the basic
// fragment shader has no instance-color input (parity gap with Metal).
layout(location = 5) in vec4 inInstanceCol0;
layout(location = 6) in vec4 inInstanceCol1;
layout(location = 7) in vec4 inInstanceCol2;
layout(location = 8) in vec4 inInstanceCol3;

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
    mat4 instanceMatrix = mat4(inInstanceCol0, inInstanceCol1, inInstanceCol2, inInstanceCol3);
    mat4 model = pc.model * instanceMatrix;

    vec4 worldPos = model * vec4(inPosition, 1.0);
    gl_Position = pc.viewProjection * worldPos;

    fragViewDepth = gl_Position.w;

    fragWorldPos = worldPos.xyz;

    mat3 normalMatrix = mat3(model);
    fragWorldNormal = normalize(normalMatrix * inNormal);
    fragWorldTangent = vec4(normalize(normalMatrix * inTangent.xyz), inTangent.w);
    fragUV0 = inUV0;
    fragUV1 = inUV1;
    fragColor = vec4(1.0);
}
