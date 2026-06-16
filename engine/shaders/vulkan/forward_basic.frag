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

// Set 1: material texture slots. Only baseColor (0) and emissive (5) are
// sampled by this basic shader; the rest are bound to a white fallback.
layout(set = 1, binding = 0) uniform sampler2D baseColorMap;
layout(set = 1, binding = 5) uniform sampler2D emissiveMap;

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
const uint FLAG_ALPHA_TEST = 1u << 0;

vec2 applyUvTransform(vec2 uv, vec4 row0, vec4 row1) {
    vec3 h = vec3(uv, 1.0);
    return vec2(dot(h, row0.xyz), dot(h, row1.xyz));
}

// Distance attenuation: inverse-square with a smooth range cutoff, or linear
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

void main() {
    vec2 uv = applyUvTransform(fragUV0, material.baseColorTransform0, material.baseColorTransform1);
    vec4 texColor = texture(baseColorMap, uv);

    vec4 albedo = material.baseColor * texColor;

    if ((material.flags & FLAG_ALPHA_TEST) != 0u && albedo.a < material.alphaCutoff) {
        discard;
    }

    vec3 N = normalize(fragWorldNormal);
    vec3 V = normalize(lighting.cameraPosExposure.xyz - fragWorldPos);

    // Ambient term.
    vec3 color = lighting.ambient.rgb * albedo.rgb;

    // A cheap specular lobe whose tightness scales with (1 - roughness).
    float shininess = mix(8.0, 128.0, 1.0 - clamp(material.roughnessFactor, 0.0, 1.0));
    float specScale = 1.0 - clamp(material.roughnessFactor, 0.0, 1.0);

    uint count = min(lighting.lightCount.x, 8u);
    for (uint i = 0u; i < count; ++i) {
        Light light = lighting.lights[i];
        uint type = uint(light.directionType.w + 0.5);

        vec3 L;
        float atten = 1.0;
        if (type == 0u) {
            // Directional: direction points along light travel; L points to light.
            L = normalize(-light.directionType.xyz);
        } else {
            vec3 toLight = light.positionRange.xyz - fragWorldPos;
            float dist = length(toLight);
            L = (dist > 1e-4) ? toLight / dist : vec3(0.0, 1.0, 0.0);
            atten = distanceAttenuation(dist, light.positionRange.w, light.coneParams.z);

            if (type == 2u) {
                // Spot cone falloff between inner and outer cosines.
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

        vec3 radiance = light.colorIntensity.rgb * light.colorIntensity.w * atten;

        // Lambert diffuse.
        vec3 diffuse = albedo.rgb * NdotL;

        // Blinn-Phong specular, suppressed as roughness rises.
        vec3 H = normalize(L + V);
        float spec = pow(max(dot(N, H), 0.0), shininess) * specScale;

        color += radiance * (diffuse + vec3(spec));
    }

    // Emissive (color × map; map defaults to white, color defaults to black).
    color += material.emissiveColor.rgb * texture(emissiveMap, uv).rgb;

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
