#version 450

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragWorldNormal;
layout(location = 2) in vec2 fragUV0;
layout(location = 3) in vec2 fragUV1;
layout(location = 4) in vec4 fragWorldTangent;

layout(location = 0) out vec4 outColor;

// Set 0 (dynamic UBO): per-draw material. Declares the prefix of the engine's
// MaterialUniforms struct that this shader consumes — std140 offsets match the
// C++ MaterialUniforms layout exactly.
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
} material;

// Set 1: material texture slots (engine slot numbering).
layout(set = 1, binding = 0) uniform sampler2D baseColorMap;     // 0
layout(set = 1, binding = 1) uniform sampler2D normalMap;        // 1
layout(set = 1, binding = 3) uniform sampler2D metalRoughMap;    // 3 (glTF: G=rough, B=metal)
layout(set = 1, binding = 4) uniform sampler2D occlusionMap;     // 4 (R = AO)
layout(set = 1, binding = 5) uniform sampler2D emissiveMap;      // 5

// Set 2 (dynamic UBO): per-pass lighting. Matches VulkanLightingUBO.
struct Light {
    vec4 positionRange;   // xyz position, w range
    vec4 directionType;   // xyz direction, w type (0=dir, 1=point, 2=spot)
    vec4 colorIntensity;  // rgb color, w intensity
    vec4 coneParams;      // innerCos, outerCos, falloffLinear, pad
};

layout(set = 2, binding = 0) uniform LightingData {
    vec4 ambient;             // rgb ambient
    vec4 cameraPosExposure;   // xyz camera position, w exposure
    uvec4 lightCount;         // x = active light count
    Light lights[8];
    vec4 fogColorDensity;     // rgb fog color, w density
    vec4 fogStartEndType;     // start, end, type (0=off,1=linear,2=exp), pad
} lighting;

// Material flag bits (subset of the engine MaterialUniforms flags).
const uint FLAG_ALPHA_TEST   = 1u << 1;
const uint FLAG_HAS_NORMAL   = 1u << 2;
const uint FLAG_DOUBLE_SIDED = 1u << 3;
const uint FLAG_HAS_METALROUGH = 1u << 6;
const uint FLAG_HAS_OCCLUSION  = 1u << 9;
const uint FLAG_HAS_EMISSIVE   = 1u << 11;

const float PI = 3.14159265359;

vec2 applyUvTransform(vec2 uv, vec4 row0, vec4 row1) {
    vec3 h = vec3(uv, 1.0);
    return vec2(dot(h, row0.xyz), dot(h, row1.xyz));
}

// Distance attenuation: inverse-square with a smooth range window, or linear
// falloff when coneParams.z != 0 (matches the engine's falloffModeLinear).
float distanceAttenuation(float dist, float range, float linearFalloff) {
    if (range <= 0.0) {
        return 1.0;
    }
    float t = clamp(dist / range, 0.0, 1.0);
    if (linearFalloff > 0.5) {
        return clamp(1.0 - t, 0.0, 1.0);
    }
    float invSq = 1.0 / max(dist * dist, 1e-4);
    float window = clamp(1.0 - t * t * t * t, 0.0, 1.0);
    return invSq * window * window;
}

// GGX normal distribution.
float distributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

// Smith geometry term (Schlick-GGX, direct lighting k).
float geometrySmith(float NdotV, float NdotL, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float gv = NdotV / (NdotV * (1.0 - k) + k);
    float gl = NdotL / (NdotL * (1.0 - k) + k);
    return gv * gl;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    vec2 uv = applyUvTransform(fragUV0, material.baseColorTransform0, material.baseColorTransform1);

    vec4 baseSample = texture(baseColorMap, uv);
    vec4 albedo = material.baseColor * baseSample;

    if ((material.flags & FLAG_ALPHA_TEST) != 0u && albedo.a < material.alphaCutoff) {
        discard;
    }

    // Metallic-roughness (glTF packs roughness in G, metallic in B).
    float metallic = material.metallicFactor;
    float roughness = material.roughnessFactor;
    if ((material.flags & FLAG_HAS_METALROUGH) != 0u) {
        vec4 mr = texture(metalRoughMap, uv);
        roughness *= mr.g;
        metallic *= mr.b;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);

    // Ambient occlusion.
    float ao = 1.0;
    if ((material.flags & FLAG_HAS_OCCLUSION) != 0u) {
        float occ = texture(occlusionMap, uv).r;
        ao = mix(1.0, occ, material.occlusionStrength);
    }

    // Geometric normal, flipped for back faces on double-sided materials.
    vec3 N = normalize(fragWorldNormal);
    if ((material.flags & FLAG_DOUBLE_SIDED) != 0u && !gl_FrontFacing) {
        N = -N;
    }

    // Tangent-space normal mapping.
    if ((material.flags & FLAG_HAS_NORMAL) != 0u) {
        vec3 T = normalize(fragWorldTangent.xyz);
        // Re-orthonormalize (Gram-Schmidt) and build the bitangent with the
        // handedness sign carried in tangent.w.
        T = normalize(T - N * dot(N, T));
        vec3 B = cross(N, T) * fragWorldTangent.w;
        vec3 tn = texture(normalMap, uv).xyz * 2.0 - 1.0;
        tn.xy *= material.normalScale;
        N = normalize(mat3(T, B, N) * tn);
    }

    vec3 V = normalize(lighting.cameraPosExposure.xyz - fragWorldPos);
    float NdotV = max(dot(N, V), 1e-4);

    vec3 F0 = mix(vec3(0.04), albedo.rgb, metallic);
    vec3 diffuseAlbedo = albedo.rgb * (1.0 - metallic);

    vec3 color = vec3(0.0);

    uint count = min(lighting.lightCount.x, 8u);
    for (uint i = 0u; i < count; ++i) {
        Light light = lighting.lights[i];
        uint type = uint(light.directionType.w + 0.5);

        vec3 L;
        float atten = 1.0;
        if (type == 0u) {
            L = normalize(-light.directionType.xyz);
        } else {
            vec3 toLight = light.positionRange.xyz - fragWorldPos;
            float dist = length(toLight);
            L = (dist > 1e-4) ? toLight / dist : vec3(0.0, 1.0, 0.0);
            atten = distanceAttenuation(dist, light.positionRange.w, light.coneParams.z);
            if (type == 2u) {
                float cd = dot(normalize(-light.directionType.xyz), L);
                float spot = clamp((cd - light.coneParams.y) /
                                   max(light.coneParams.x - light.coneParams.y, 1e-4), 0.0, 1.0);
                atten *= spot * spot;
            }
        }

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0 || atten <= 0.0) {
            continue;
        }

        vec3 H = normalize(L + V);
        float NdotH = max(dot(N, H), 0.0);
        float VdotH = max(dot(V, H), 0.0);

        float D = distributionGGX(NdotH, roughness);
        float G = geometrySmith(NdotV, NdotL, roughness);
        vec3 F = fresnelSchlick(VdotH, F0);

        vec3 specular = (D * G) * F / max(4.0 * NdotV * NdotL, 1e-4);
        vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

        vec3 radiance = light.colorIntensity.rgb * light.colorIntensity.w * atten;
        color += (kD * diffuseAlbedo / PI + specular) * radiance * NdotL;
    }

    // Ambient (cheap pre-IBL approximation): diffuse + a Fresnel-weighted
    // specular floor so metals are not pitch black without an environment.
    vec3 ambientDiffuse = lighting.ambient.rgb * diffuseAlbedo;
    vec3 ambientSpecular = lighting.ambient.rgb * F0;
    color += (ambientDiffuse + ambientSpecular) * ao;

    // Emissive.
    vec3 emissive = material.emissiveColor.rgb;
    if ((material.flags & FLAG_HAS_EMISSIVE) != 0u) {
        emissive *= texture(emissiveMap, uv).rgb;
    }
    color += emissive;

    // Fog (linear or exponential) toward the fog color.
    float fogType = lighting.fogStartEndType.z;
    if (fogType > 0.5) {
        float dist = length(lighting.cameraPosExposure.xyz - fragWorldPos);
        float f;
        if (fogType < 1.5) {
            f = clamp((lighting.fogStartEndType.y - dist) /
                      max(lighting.fogStartEndType.y - lighting.fogStartEndType.x, 1e-4), 0.0, 1.0);
        } else {
            float d = dist * lighting.fogColorDensity.w;
            f = clamp(exp(-d * d), 0.0, 1.0);
        }
        color = mix(lighting.fogColorDensity.rgb, color, f);
    }

    // Exposure then display-gamma encode (swapchain is a linear UNORM target,
    // matching the Metal BGRA8Unorm drawable).
    color *= lighting.cameraPosExposure.w;
    color = pow(color, vec3(1.0 / 2.2));

    outColor = vec4(color, albedo.a);
}
