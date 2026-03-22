#version 450

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragWorldNormal;
layout(location = 2) in vec2 fragUV0;
layout(location = 3) in vec2 fragUV1;
layout(location = 4) in vec4 fragWorldTangent;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform MaterialData {
    vec4 baseColor;
    vec4 emissiveColor;
    uint flags;
    uint occludeSpecularMode;
    float alphaCutoff;
    float metallicFactor;
    float roughnessFactor;
    float normalScale;
    float occlusionStrength;
    float occludeSpecularIntensity;
} material;

layout(set = 1, binding = 0) uniform sampler2D baseColorMap;

void main() {
    vec4 texColor = texture(baseColorMap, fragUV0);
    vec3 color = material.baseColor.rgb * texColor.rgb;

    vec3 N = normalize(fragWorldNormal);
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float NdotL = max(dot(N, lightDir), 0.0);

    vec3 ambient = vec3(0.15);
    color = color * (ambient + NdotL * vec3(0.85));

    color = pow(color, vec3(1.0 / 2.2));

    outColor = vec4(color, material.baseColor.a * texColor.a);
}
