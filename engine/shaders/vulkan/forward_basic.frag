#version 450

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragWorldNormal;
layout(location = 2) in vec2 fragUV0;
layout(location = 3) in vec2 fragUV1;
layout(location = 4) in vec4 fragWorldTangent;
layout(location = 5) in float fragViewDepth;

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
    vec4 coneParams;      // innerCos, outerCos, falloffLinear, localShadowIndex(-1/0/1)
};

layout(set = 2, binding = 0) uniform LightingData {
    vec4 ambient;             // rgb ambient
    vec4 cameraPosExposure;   // xyz camera position, w exposure
    uvec4 lightCount;         // x = active light count
    Light lights[8];
    vec4 fogColorDensity;     // rgb fog color, w density
    vec4 fogStartEndType;     // start, end, type (0=off,1=linear,2=exp), pad
    vec4 envParams;           // skyboxIntensity, hasEnvAtlas, encoding, skyboxMip
    mat4 shadowMatrices[4];   // per-cascade world → shadow-atlas UV + depth
    vec4 shadowCascadeDistances; // per-cascade far split (view-space depth)
    vec4 shadowParams;        // enabled, numCascades, depthBias, strength
    vec4 shadowParams2;       // normalBias, cascadeBlend, toneMapping, enableNormalMaps
    mat4 localShadowMatrix0;  // spot slot 0: world → shadow UV + depth
    mat4 localShadowMatrix1;  // spot slot 1
    vec4 localShadowParams0;  // depthBias, normalBias, intensity, isOmni
    vec4 localShadowParams1;
    vec4 omniShadowParams0;   // near, far, depthBias, intensity
    vec4 omniShadowParams1;
} lighting;

// Set 3: scene textures. Binding 0 = equirectangular environment atlas
// (IBL irradiance + roughness mips + skybox source). Binding 1 = directional
// cascaded shadow-map depth atlas. Bindings 2-3 = local spot-light 2D depth
// maps; bindings 4-5 = omni point-light cubemap depth maps. All shadow maps
// are sampled through a NEAREST clamp sampler with manual depth comparison.
layout(set = 3, binding = 0) uniform sampler2D envAtlas;
layout(set = 3, binding = 1) uniform sampler2D shadowMap;
layout(set = 3, binding = 2) uniform sampler2D localShadowMap0;
layout(set = 3, binding = 3) uniform sampler2D localShadowMap1;
layout(set = 3, binding = 4) uniform samplerCube omniShadowCube0;
layout(set = 3, binding = 5) uniform samplerCube omniShadowCube1;

// 3×3 percentage-closer filter: average binary depth comparisons over the
// texel neighbourhood.  `receiver` is the (biased) light-space depth of the
// shaded point; a texel is lit when its stored occluder depth is no nearer.
float pcf3x3(sampler2D tex, vec2 uv, float receiver) {
    vec2 texel = 1.0 / vec2(textureSize(tex, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float occluder = texture(tex, uv + vec2(x, y) * texel).r;
            sum += (receiver <= occluder) ? 1.0 : 0.0;
        }
    }
    return sum / 9.0;
}

// Directional cascaded-shadow visibility (1 = lit, 0 = shadowed) for a world
// position, using view-space depth to pick the cascade.  Returns 1.0 when
// shadows are disabled or the point falls outside every cascade.
float sampleDirectionalShadow(vec3 worldPos, float viewDepth, vec3 N, vec3 L) {
    if (lighting.shadowParams.x < 0.5) {
        return 1.0;
    }
    int cascadeCount = int(lighting.shadowParams.y);

    // Cascade = number of split distances the fragment is beyond.
    int cascade = 0;
    for (int i = 0; i < cascadeCount - 1; ++i) {
        if (viewDepth > lighting.shadowCascadeDistances[i]) {
            cascade = i + 1;
        }
    }

    // World-space normal bias, scaled by grazing angle to curb peter-panning
    // on directly-lit faces while offsetting shadow-acne on grazing ones.
    float ndl = clamp(dot(N, L), 0.0, 1.0);
    float sinAngle = sqrt(max(1.0 - ndl * ndl, 0.0));
    vec3 biased = worldPos + N * (lighting.shadowParams2.x * sinAngle);

    // Project into the cascade's atlas: the matrix bakes projection, view,
    // NDC→UV, and Z[0,1]; a perspective divide yields UV + depth directly.
    vec4 sc = lighting.shadowMatrices[cascade] * vec4(biased, 1.0);
    if (sc.w <= 0.0) {
        return 1.0;
    }
    vec3 coord = sc.xyz / sc.w;
    // No V flip: the cascade matrix bakes the Metal top-left atlas convention,
    // and the negative-height viewport used for every Vulkan pass (including
    // shadow renders) stores the map in exactly that orientation. A whole-atlas
    // 1-V flip here would sample the wrong cascade quadrant for any multi-
    // cascade layout.
    if (coord.x < 0.0 || coord.x > 1.0 || coord.y < 0.0 || coord.y > 1.0 ||
        coord.z < 0.0 || coord.z > 1.0) {
        return 1.0;
    }

    // Manual 3×3 PCF, then scale by shadow strength (0 = no shadow, 1 = full).
    float receiver = coord.z - lighting.shadowParams.z;
    float visible = pcf3x3(shadowMap, coord.xy, receiver);
    return mix(1.0, visible, lighting.shadowParams.w);
}

// Spot-light 2D shadow visibility (1 = lit, 0 = shadowed).  Mirrors the
// directional path but with a single per-light matrix, bias, and intensity.
float sampleSpotShadow(int slot, vec3 worldPos, vec3 N, vec3 L) {
    mat4 m  = (slot == 0) ? lighting.localShadowMatrix0 : lighting.localShadowMatrix1;
    vec4 sp = (slot == 0) ? lighting.localShadowParams0 : lighting.localShadowParams1;

    // World-space normal bias, scaled by grazing angle (matches Metal).
    float ndl = clamp(dot(N, L), 0.0, 1.0);
    float sinAngle = sqrt(max(1.0 - ndl * ndl, 0.0));
    vec3 biased = worldPos + N * (sp.y * sinAngle);

    vec4 sc = m * vec4(biased, 1.0);
    if (sc.w <= 0.0) {
        return 1.0;
    }
    vec3 coord = sc.xyz / sc.w;
    // No V flip — same reasoning as the CSM path: the spot matrix bakes the
    // Metal orientation and the negative-height viewport reproduces it.
    if (coord.x < 0.0 || coord.x > 1.0 || coord.y < 0.0 || coord.y > 1.0 ||
        coord.z < 0.0 || coord.z > 1.0) {
        return 1.0;
    }

    float receiver = coord.z - sp.x;
    float visible = (slot == 0)
        ? pcf3x3(localShadowMap0, coord.xy, receiver)
        : pcf3x3(localShadowMap1, coord.xy, receiver);
    // Local shadows blend toward (1 - intensity) when occluded.
    return mix(1.0 - clamp(sp.z, 0.0, 1.0), 1.0, visible);
}

// Omni (point) light cubemap shadow visibility.  The light→fragment direction
// selects the cubemap face; the stored depth is the perspective-projected
// distance along the dominant axis, reconstructed here from the linear
// distance to match the shadow render's projection.
float sampleOmniShadow(int slot, vec3 worldPos, vec3 lightPos) {
    vec4 op = (slot == 0) ? lighting.omniShadowParams0 : lighting.omniShadowParams1;
    float nearV = op.x, farV = op.y, bias = op.z, intensity = op.w;

    vec3 dir = worldPos - lightPos;
    float d = max(abs(dir.x), max(abs(dir.y), abs(dir.z)));
    float denom = (farV - nearV) * d;
    float compareValue = farV * (d - nearV) / max(denom, 1e-6) - bias;

    float occluder = (slot == 0) ? texture(omniShadowCube0, dir).r
                                 : texture(omniShadowCube1, dir).r;
    float visible = (compareValue <= occluder) ? 1.0 : 0.0;
    return mix(1.0 - clamp(intensity, 0.0, 1.0), 1.0, visible);
}

// ── Environment atlas layout (matches engine bake / Metal common.metal) ──
const float ATLAS_SIZE = 512.0;
const float ATLAS_SEAM = 1.0 / 512.0;

// Equirectangular direction → atlas UV (atan2 azimuth, asin elevation).
vec2 dirToEquirect(vec3 dir) {
    vec2 sph = vec2(atan(dir.x, dir.z), asin(clamp(dir.y, -1.0, 1.0)));
    vec2 uv = sph / vec2(6.28318530718, 3.14159265359) + 0.5;
    return vec2(uv.x, 1.0 - uv.y);
}

// Map a [0,1] uv into a packed sub-rect (x,y,w,h), insetting by the 1px seam.
vec2 mapRect(vec2 uv, vec4 rect) {
    return vec2(mix(rect.x + ATLAS_SEAM, rect.x + rect.z - ATLAS_SEAM, uv.x),
                mix(rect.y + ATLAS_SEAM, rect.y + rect.w - ATLAS_SEAM, uv.y));
}

// Lambert irradiance sub-rect: 64×32 region at (128,384).
vec2 mapAmbientUv(vec2 uv) {
    return mapRect(uv, vec4(128.0 / ATLAS_SIZE, 384.0 / ATLAS_SIZE,
                            64.0 / ATLAS_SIZE, 32.0 / ATLAS_SIZE));
}

// Prefiltered roughness mip `level` down the left edge: rect (0, 1-t, t, t/2).
vec2 mapRoughnessUv(vec2 uv, float level) {
    float t = 1.0 / exp2(level);
    return mapRect(uv, vec4(0.0, 1.0 - t, t, t * 0.5));
}

vec3 decodeRGBP(vec4 raw) { vec3 c = raw.rgb * (-raw.a * 7.0 + 8.0); return c * c; }
vec3 decodeRGBM(vec4 raw) { vec3 c = (8.0 * raw.a) * raw.rgb; return c * c; }
vec3 srgbToLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }

vec3 decodeEnv(vec4 raw) {
    uint enc = uint(lighting.envParams.z + 0.5);
    if (enc == 1u) return decodeRGBP(raw);
    if (enc == 2u) return decodeRGBM(raw);
    return srgbToLinear(raw.rgb);
}

// ── Tonemapping (parity with the Metal common.metal toneMap dispatch) ──
// Modes match scene/constants.h: 0=linear, 1=filmic, 2=hejl, 3=aces,
// 4=aces2, 5=neutral, 6=none. Applied before display-gamma encode.

vec3 toneMapAcesFit(vec3 x) {
    const float tA = 2.51, tB = 0.03, tC = 2.43, tD = 0.59, tE = 0.14;
    return (x * (tA * x + tB)) / (x * (tC * x + tD) + tE);
}

vec3 uncharted2Tonemap(vec3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 toneMapFilmic(vec3 color) {
    const float W = 11.2;
    color = uncharted2Tonemap(color * 2.0);
    return color * (1.0 / uncharted2Tonemap(vec3(W)));
}

vec3 toneMapHejl(vec3 color) {
    const float A = 0.22, B = 0.3, C = 0.1, D = 0.2, E = 0.01, F = 0.3;
    const float scl = 1.25;
    vec3 h = max(vec3(0.0), color - 0.004);
    return (h * ((scl * A) * h + scl * (C * B)) + scl * (D * E))
         / (h * (A * h + B) + (D * F))
         - scl * (E / F);
}

vec3 RRTAndODTFit(vec3 v) {
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

vec3 toneMapAces2(vec3 color) {
    // GLSL mat3 constructors take COLUMNS; these are the transposes of the
    // row-major matrices in common.metal (which multiplies vector * matrix).
    const mat3 ACESInputMat = mat3(
        0.59719, 0.07600, 0.02840,
        0.35458, 0.90834, 0.13383,
        0.04823, 0.01566, 0.83777);
    const mat3 ACESOutputMat = mat3(
         1.60475, -0.10208, -0.00327,
        -0.53108,  1.10813, -0.07276,
        -0.07367, -0.00605,  1.07602);
    color /= 0.6;
    color = ACESInputMat * color;
    color = RRTAndODTFit(color);
    color = ACESOutputMat * color;
    return clamp(color, vec3(0.0), vec3(1.0));
}

vec3 toneMapNeutral(vec3 color) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;
    float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, vec3(newPeak), g);
}

// Exposure-scaled color -> tonemapped color, by scene mode.
vec3 applyToneMap(vec3 color) {
    int mode = int(lighting.shadowParams2.z + 0.5);
    if (mode == 1) return toneMapFilmic(color);
    if (mode == 2) return toneMapHejl(color);
    if (mode == 3) return toneMapAcesFit(color);
    if (mode == 4) return toneMapAces2(color);
    if (mode == 5) return toneMapNeutral(color);
    return color; // LINEAR (0) and NONE (6): exposure only
}

// Material flag bits (subset of the engine MaterialUniforms flags).
const uint FLAG_ALPHA_TEST   = 1u << 1;
const uint FLAG_HAS_NORMAL   = 1u << 2;
const uint FLAG_DOUBLE_SIDED = 1u << 3;
const uint FLAG_HAS_METALROUGH = 1u << 6;
const uint FLAG_SKYBOX         = 1u << 8;
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
    // Skybox: sample the environment along the view direction and output it
    // directly — no surface lighting.  The sky mesh is centered on the camera,
    // so the world-space position relative to the camera is the view ray.
    if ((material.flags & FLAG_SKYBOX) != 0u) {
        vec3 dir = normalize(fragWorldPos - lighting.cameraPosExposure.xyz);
        vec3 sky;
        if (lighting.envParams.y > 0.5) {
            float intensity = max(lighting.envParams.x, 0.0);
            sky = decodeEnv(texture(envAtlas, mapRoughnessUv(dirToEquirect(dir),
                                    max(lighting.envParams.w, 0.0)))) * intensity;
        } else {
            sky = material.baseColor.rgb;
        }
        sky *= lighting.cameraPosExposure.w;            // exposure
        sky = applyToneMap(sky);
        outColor = vec4(pow(max(sky, vec3(0.0)), vec3(1.0 / 2.2)), 1.0); // display-gamma encode
        return;
    }

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

    // Tangent-space normal mapping (shadowParams2.w = global enable toggle).
    if ((material.flags & FLAG_HAS_NORMAL) != 0u && lighting.shadowParams2.w > 0.5) {
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
            // Directional light is the CSM shadow caster.
            atten = sampleDirectionalShadow(fragWorldPos, fragViewDepth, N, L);
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

            // Local light shadows: coneParams.w carries the shadow slot
            // (-1 = no shadow, 0/1 = local caster).  Point lights use the
            // cubemap path, spot lights the projected 2D path.
            float shadowIndex = light.coneParams.w;
            if (shadowIndex >= 0.0) {
                int slot = int(shadowIndex + 0.5);
                if (type == 1u) {
                    atten *= sampleOmniShadow(slot, fragWorldPos, light.positionRange.xyz);
                } else if (type == 2u) {
                    atten *= sampleSpotShadow(slot, fragWorldPos, N, L);
                }
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

    // Indirect lighting.  With an environment atlas: image-based diffuse
    // irradiance + roughness-prefiltered specular reflection.  Without one:
    // a flat ambient term plus a Fresnel-weighted specular floor so metals
    // aren't pitch black.
    vec3 indirect;
    if (lighting.envParams.y > 0.5) {
        float intensity = max(lighting.envParams.x, 0.0);

        // Diffuse irradiance (the negate-X matches the engine's atlas lookup
        // handedness).
        vec3 diffDir = vec3(-N.x, N.y, N.z);
        vec3 irradiance = decodeEnv(texture(envAtlas, mapAmbientUv(dirToEquirect(diffDir)))) * intensity;

        // Specular: reflect, pick a roughness mip, trilinear between levels.
        vec3 R = reflect(-V, N);
        vec3 specDir = vec3(-R.x, R.y, R.z);
        vec2 envUv = dirToEquirect(specDir);
        float level = clamp(roughness * 5.0, 0.0, 5.0);
        float l0 = floor(level);
        vec3 envA = decodeEnv(texture(envAtlas, mapRoughnessUv(envUv, l0)));
        vec3 envB = decodeEnv(texture(envAtlas, mapRoughnessUv(envUv, l0 + 1.0)));
        vec3 prefiltered = mix(envA, envB, level - l0) * intensity;

        // Schlick-roughness Fresnel for the environment term.
        vec3 Fr = F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, 5.0);
        vec3 kD = (vec3(1.0) - Fr) * (1.0 - metallic);

        indirect = kD * irradiance * diffuseAlbedo + prefiltered * Fr;
    } else {
        indirect = lighting.ambient.rgb * diffuseAlbedo + lighting.ambient.rgb * F0;
    }
    color += indirect * ao;

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

    // Exposure, tonemap, then display-gamma encode (swapchain is a linear
    // UNORM target, matching the Metal BGRA8Unorm drawable).
    color *= lighting.cameraPosExposure.w;
    color = applyToneMap(color);
    color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));

    outColor = vec4(color, albedo.a);
}
