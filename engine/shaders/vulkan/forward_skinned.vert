#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV0;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec2 inUV1;
layout(location = 11) in vec4 inBlendWeights;
layout(location = 12) in vec4 inBlendIndices;

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
    uvec4 joints = uvec4(inBlendIndices);
    mat4 skin = inBlendWeights.x * palette.matrices[joints.x]
              + inBlendWeights.y * palette.matrices[joints.y]
              + inBlendWeights.z * palette.matrices[joints.z]
              + inBlendWeights.w * palette.matrices[joints.w];
    mat4 modelSkin = pc.model * skin;
    vec4 worldPos = modelSkin * vec4(inPosition, 1.0);
    gl_Position = pc.viewProjection * worldPos;
    mat3 normalMatrix = mat3(modelSkin);
    fragWorldPos = worldPos.xyz;
    fragWorldNormal = normalize(normalMatrix * inNormal);
    fragWorldTangent =
        vec4(normalize(normalMatrix * inTangent.xyz), inTangent.w);
    fragUV0 = inUV0;
    fragUV1 = inUV1;
    fragViewDepth = gl_Position.w;
    fragColor = vec4(1.0);
}
