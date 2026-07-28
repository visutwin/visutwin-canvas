#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV0;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec2 inUV1;

#include "shader_features.glsl"

// Declared verbatim from forward.frag: MoltenVK miscompiles a UBO whose member
// list differs between stages, so these two blocks must stay identical.
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
    vec4 baseColorTransform0;
    vec4 baseColorTransform1;
    vec4 normalTransform0;
    vec4 normalTransform1;
    vec4 metalRoughTransform0;
    vec4 metalRoughTransform1;
    vec4 occlusionTransform0;
    vec4 occlusionTransform1;
    vec4 emissiveTransform0;
    vec4 emissiveTransform1;
    float clearCoatFactor;
    float clearCoatRoughness;
    float clearCoatBumpiness;
    float heightMapFactor;
    float anisotropy;
    float transmissionFactor;
    float refractionIndex;
    float thickness;
    vec4 sheenColor;
    vec4 iridescenceParams;
    vec4 specGlossParams;
    vec4 detailDisplacementParams;
    vec4 detailNormalTransform0;
    vec4 detailNormalTransform1;
    vec4 attenuationParams;
    vec4 dispersionParams;
} material;

// Displacement height map: a separate image in the vertex stage sharing the
// material sampler, so it costs no fragment-stage sampler slot.
layout(set = 1, binding = 25) uniform texture2D displacementImage;
layout(set = 1, binding = 24) uniform sampler materialExtraSampler;

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
    vec3 localPos = inPosition;
    // Vertex displacement along the local normal from a height map. Explicit LOD
    // is required in the vertex stage (no implicit derivatives).
    // detailDisplacementParams: y = displacementScale, z = displacementBias.
    // DEVIATION: standard vertex path only, matching the Metal port — not the
    // instanced / dynamic-batch / skinned specializations.
    if (vtFeatureEnabled(VT_FEATURE_DISPLACEMENT_BIT)) {
        float height = textureLod(
            sampler2D(displacementImage, materialExtraSampler), inUV0, 0.0).r;
        localPos += inNormal * ((height - material.detailDisplacementParams.z) *
            material.detailDisplacementParams.y);
    }
    vec4 worldPos = pc.model * vec4(localPos, 1.0);
    gl_Position = pc.viewProjection * worldPos;

    // clip.w == view-space Z for a standard perspective projection — used for
    // cascaded-shadow cascade selection in the fragment stage.
    fragViewDepth = gl_Position.w;

    fragWorldPos = worldPos.xyz;

    mat3 normalMatrix = mat3(pc.model);
    fragWorldNormal = normalize(normalMatrix * inNormal);
    fragWorldTangent = vec4(normalize(normalMatrix * inTangent.xyz), inTangent.w);
    fragUV0 = inUV0;
    fragUV1 = inUV1;
    fragColor = vec4(1.0);
}
