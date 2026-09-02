layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragWorldNormal;
layout(location = 2) in vec2 fragUV0;
layout(location = 3) in vec2 fragUV1;
layout(location = 4) in vec4 fragWorldTangent;
layout(location = 5) in float fragViewDepth;
layout(location = 6) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

// Set 0 (dynamic UBO): per-draw material. Declares the prefix of the engine's
// MaterialUniforms struct that this shader consumes — std140 offsets match the
// C++ MaterialUniforms layout exactly.
// Emitted from scene/materials/materialUniformFields.h (build time:
// shader_material.glsl; runtime: ProgramLibrary).
#include "shader_material.glsl"

// Set 1: material texture slots (engine slot numbering).
layout(set = 1, binding = 0) uniform sampler2D baseColorMap;     // 0
layout(set = 1, binding = 1) uniform sampler2D normalMap;        // 1
layout(set = 1, binding = 3) uniform sampler2D metalRoughMap;    // 3 (glTF: G=rough, B=metal)
layout(set = 1, binding = 4) uniform sampler2D occlusionMap;     // 4 (R = AO)
layout(set = 1, binding = 5) uniform sampler2D emissiveMap;      // 5
layout(set = 1, binding = 19) uniform sampler2D lightMap;        // 19
// 17 and 23 are separate images reading through the shared sampler at 24, so
// they cost no extra per-stage sampler slot (the device allows only 16).
layout(set = 1, binding = 17) uniform texture2D heightMapImage;   // 17 (parallax)
layout(set = 1, binding = 23) uniform texture2D detailNormalImage;// 23
layout(set = 1, binding = 24) uniform sampler materialExtraSampler;
#define heightMap    sampler2D(heightMapImage, materialExtraSampler)
#define detailNormal sampler2D(detailNormalImage, materialExtraSampler)

// Set 2 (dynamic UBO): per-pass lighting. Matches VulkanLightingUBO.
struct Light {
    vec4 positionRange;   // xyz position, w range
    vec4 directionType;   // xyz direction, w type (0=dir, 1=point, 2=spot)
    vec4 colorIntensity;  // rgb color, w intensity
    vec4 coneParams;      // innerCos, outerCos, falloffLinear, localShadowIndex(-1/0/1)
    vec4 areaRightHalfWidth;
    vec4 areaUpHalfHeight;
    vec4 cookieFlags;     // hasCookie, cookie slot, CookieChannel, cookieFalloff
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
    vec4 pcssParams;          // filterSamples, blockerSamples, penumbraSize, penumbraFalloff
    vec4 pcssCascadeRadii;       // per-cascade shadow-camera ortho half-extent
    vec4 pcssCascadeDepthRanges; // per-cascade caster depth span (far - near)
    mat4 localShadowMatrix0;  // spot slot 0: world → shadow UV + depth
    mat4 localShadowMatrix1;  // spot slot 1
    vec4 localShadowParams0;  // depthBias, normalBias, intensity, isOmni
    vec4 localShadowParams1;
    vec4 omniShadowParams0;   // near, far, depthBias, intensity
    vec4 omniShadowParams1;
    vec4 localShadowPcss0;    // searchArea UV (0 = off), near, far, pad
    vec4 localShadowPcss1;
    // Light cookies: spot slots carry a world → cookie-UV projection, omni slots
    // the light's world transform (its rotation maps light→fragment into cube space).
    mat4 cookieMatrix2D0;
    mat4 cookieMatrix2D1;
    mat4 cookieMatrixCube0;
    mat4 cookieMatrixCube1;
    vec4 cookieParams2D0;     // intensity, cookieFalloff, CookieChannel, pad
    vec4 cookieParams2D1;
    vec4 cookieParamsCube0;
    vec4 cookieParamsCube1;
    vec4 skyParams2;          // xyz sky dome center, w flags: bit0 cubemap, bit1 dome
    vec4 ambientSH[9];
    vec4 clusterBoundsMin;
    vec4 clusterBoundsRange;
    vec4 clusterCellsCountByBoundsSize;
    uvec4 clusterParams;
    uvec4 clusterParams2;
    vec4 reflectionProbeBoxMin;
    vec4 reflectionProbeBoxMax;
    vec4 reflectionProbePosition;
    vec4 reflectionProbeParams;
    mat4 viewProjection;      // world → clip, for the SSR screen-space march
    vec4 cameraNearFar;       // near, far, colourGrabBound, depthGrabBound
    // Nishita atmosphere. DEVIATION: Metal binds this as its own fragment
    // buffer (slot 9); here it rides the lighting UBO, which is scene-global
    // and uploaded only when dirty, so it costs no extra descriptor set.
    vec4 atmoPlanetCenterAndRadius;   // xyz = planet centre (camera-local), w = radius (m)
    vec4 atmoRadiusAndSunIntensity;   // x = outer radius (m), y = sun intensity, z = cos(sun disk half-angle)
    vec4 atmoRayleighCoeffAndScale;   // xyz = Rayleigh coefficients (per m), w = scale height (m)
    vec4 atmoMieCoeffAndScale;        // x = Mie coefficient, y = scale height (m), z = HG g
    vec4 atmoSunDirection;            // xyz = normalized sun direction (camera-local)
    vec4 atmoCameraAltitudeAndParams; // x = altitude (m), y = primary steps, z = secondary steps
    // Blurred planar reflection.
    vec4 screenInvResolution;         // xy = 1/viewport, zw = viewport
    vec4 reflectionParams;            // x = intensity, y = blur, z = fadeStrength, w = angleFade
    vec4 reflectionFadeColor;         // xyz = colour reflections fade into
    vec4 reflectionDepthParams;       // x = planeDistance, y = heightRange,
                                      // z = colour map bound, w = depth map bound
    uvec4 flagsAndPad;                // [1] = DebugShaderPass mode (see below)
} lighting;

// Debug shader passes. Must match scene/constants.h :: DebugShaderPass. The
// active mode arrives in flagsAndPad[1], so all modes share one compiled
// variant (VT_FEATURE_DEBUG_PASS) and switching needs no recompile.
const uint VT_DEBUGPASS_NONE        = 0u;
const uint VT_DEBUGPASS_ALBEDO      = 1u;
const uint VT_DEBUGPASS_WORLDNORMAL = 2u;
const uint VT_DEBUGPASS_OPACITY     = 3u;
const uint VT_DEBUGPASS_SPECULARITY = 4u;
const uint VT_DEBUGPASS_GLOSS       = 5u;
const uint VT_DEBUGPASS_METALNESS   = 6u;
const uint VT_DEBUGPASS_AO          = 7u;
const uint VT_DEBUGPASS_EMISSION    = 8u;
const uint VT_DEBUGPASS_LIGHTING    = 9u;
const uint VT_DEBUGPASS_UV0         = 10u;

struct ClusterLight {
    vec4 positionRange;
    vec4 directionSpot;
    vec4 colorIntensity;
    vec4 params;
    mat4 shadowMatrix;
    vec4 shadowData;
};
layout(std430, set = 5, binding = 0) readonly buffer ClusterLights {
    ClusterLight values[];
} clusterLights;
layout(std430, set = 5, binding = 1) readonly buffer ClusterCells {
    uint values[];
} clusterCells;

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
// Bindings 6-11 are declared as separate images sharing two sampler objects.
// A combined image sampler costs a per-stage sampler slot, and this device caps
// those at 16 (a hard Metal limit MoltenVK inherits) — six more combined
// bindings would exceed it. Separate images cost none, so only the two sampler
// descriptors below are charged. The aliases keep every call site unchanged.
layout(set = 3, binding = 6) uniform textureCube skyboxCubeImage;
layout(set = 3, binding = 7) uniform textureCube reflectionProbeCubeImage;
// LTC area-light lookup tables (64×64 RGBA16F).
// 8 = inverse LTC matrix columns, 9 = Fresnel/geometry magnitudes.
layout(set = 3, binding = 8) uniform texture2D areaLightLut1Image;
layout(set = 3, binding = 9) uniform texture2D areaLightLut2Image;
// Mid-frame scene grabs for screen-space reflections. The depth grab is a copy:
// the live depth buffer is still attached while the reflective surface draws, so
// it cannot be sampled directly.
layout(set = 3, binding = 10) uniform texture2D ssrSceneColorImage;
layout(set = 3, binding = 11) uniform texture2D ssrSceneDepthImage;
layout(set = 3, binding = 12) uniform sampler linearClampSampler;
layout(set = 3, binding = 13) uniform sampler nearestClampSampler;
// Clustered spot-shadow atlas: one depth slice per shadow-casting clustered
// spot, indexed by ClusterLight.shadowData.w. Declared after the samplers so
// the two sampler descriptors keep their existing bindings; a separate image
// again costs no per-stage sampler slot.
layout(set = 3, binding = 14) uniform texture2DArray clusterShadowAtlasImage;
// Blurred planar reflection: the mirrored scene colour rendered by the
// reflection camera, and the distance-from-plane map from the depth camera.
layout(set = 3, binding = 15) uniform texture2D planarReflectionImage;
layout(set = 3, binding = 16) uniform texture2D planarReflectionDepthImage;
// Light cookies: 2D for spot lights, cubemap for omni, two slots each. Separate
// images for the same reason as the block above — the per-stage sampler slots
// are nearly exhausted, and these want the plain linear clamp sampler anyway.
layout(set = 3, binding = 17) uniform texture2D cookieImage2D0;
layout(set = 3, binding = 18) uniform texture2D cookieImage2D1;
layout(set = 3, binding = 19) uniform textureCube cookieImageCube0;
layout(set = 3, binding = 20) uniform textureCube cookieImageCube1;

#define skyboxCube        samplerCube(skyboxCubeImage, linearClampSampler)
#define reflectionProbeCube samplerCube(reflectionProbeCubeImage, linearClampSampler)
#define areaLightLut1     sampler2D(areaLightLut1Image, linearClampSampler)
#define areaLightLut2     sampler2D(areaLightLut2Image, linearClampSampler)
#define ssrSceneColor     sampler2D(ssrSceneColorImage, linearClampSampler)
// Point-sampled: depth formats commonly lack linear filter support, and
// interpolating depth across a silhouette invents surfaces that aren't there.
#define ssrSceneDepth     sampler2D(ssrSceneDepthImage, nearestClampSampler)
// Point-sampled for the same reason as every other shadow map here: manual
// depth comparison per tap, so filtering must not blend across occluders.
#define clusterShadowAtlas sampler2DArray(clusterShadowAtlasImage, nearestClampSampler)
#define cookie2D0         sampler2D(cookieImage2D0, linearClampSampler)
#define cookie2D1         sampler2D(cookieImage2D1, linearClampSampler)
#define cookieCube0       samplerCube(cookieImageCube0, linearClampSampler)
#define cookieCube1       samplerCube(cookieImageCube1, linearClampSampler)
// Linear-filtered: both are ordinary colour renders, and the Poisson taps below
// want interpolation between texels.
#define planarReflection      sampler2D(planarReflectionImage, linearClampSampler)
#define planarReflectionDepth sampler2D(planarReflectionDepthImage, linearClampSampler)

const float PI = 3.14159265359;

