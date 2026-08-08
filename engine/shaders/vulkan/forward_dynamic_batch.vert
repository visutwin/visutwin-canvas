#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV0;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec2 inUV1;
layout(location = 5) in float inPaletteIndex;

layout(std430, set = 4, binding = 0) readonly buffer PaletteData {
    mat4 matrices[];
} palette;

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
    mat4 model = palette.matrices[uint(inPaletteIndex)];
    vec4 worldPos = model * vec4(inPosition, 1.0);
    gl_Position = pc.viewProjection * worldPos;
    // GL [-1,1] -> Vulkan [0,1] clip z. See forward.vert for why.
    gl_Position.z = 0.5 * (gl_Position.z + gl_Position.w);
    mat3 normalMatrix = mat3(model);
    fragWorldPos = worldPos.xyz;
    fragWorldNormal = normalize(normalMatrix * inNormal);
    fragWorldTangent =
        vec4(normalize(normalMatrix * inTangent.xyz), inTangent.w);
    fragUV0 = inUV0;
    fragUV1 = inUV1;
    fragViewDepth = gl_Position.w;
    fragColor = vec4(1.0);
}
