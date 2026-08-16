// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Shared shader feature contract. ProgramLibrary resolves these once; Metal
// emits matching preprocessor defines and Vulkan passes the same mask through
// specialization constants in its build-time compiled SPIR-V modules.
#pragma once

#include <cstdint>

#define VT_SHADER_FEATURES(X) \
    X(BaseColorMap,              "VT_FEATURE_BASE_COLOR_MAP",               0) \
    X(NormalMap,                 "VT_FEATURE_NORMAL_MAP",                   1) \
    X(MetallicRoughnessMap,      "VT_FEATURE_METAL_ROUGHNESS_MAP",          2) \
    X(OcclusionMap,              "VT_FEATURE_OCCLUSION_MAP",                3) \
    X(EmissiveMap,               "VT_FEATURE_EMISSIVE_MAP",                 4) \
    X(AlphaTest,                 "VT_FEATURE_ALPHA_TEST",                   5) \
    X(DoubleSided,               "VT_FEATURE_DOUBLE_SIDED",                 6) \
    X(Shadows,                   "VT_FEATURE_SHADOWS",                      8) \
    X(Fog,                       "VT_FEATURE_FOG",                          9) \
    X(VertexColors,              "VT_FEATURE_VERTEX_COLORS",               10) \
    X(PointSpotAttenuation,      "VT_FEATURE_POINT_SPOT_ATTENUATION",      11) \
    X(MultiLight,                "VT_FEATURE_MULTI_LIGHT",                  12) \
    X(EnvAtlas,                  "VT_FEATURE_ENV_ATLAS",                    13) \
    X(Parallax,                  "VT_FEATURE_PARALLAX",                     14) \
    X(Clearcoat,                 "VT_FEATURE_CLEARCOAT",                    15) \
    X(Anisotropy,                "VT_FEATURE_ANISOTROPY",                   16) \
    X(Sheen,                     "VT_FEATURE_SHEEN",                        17) \
    X(Iridescence,               "VT_FEATURE_IRIDESCENCE",                  18) \
    X(Transmission,              "VT_FEATURE_TRANSMISSION",                 19) \
    X(LightClustering,           "VT_FEATURE_LIGHT_CLUSTERING",             20) \
    X(Ssao,                      "VT_FEATURE_SSAO",                         21) \
    X(LightProbes,               "VT_FEATURE_LIGHT_PROBES",                 22) \
    X(Skinning,                  "VT_FEATURE_SKINNING",                     23) \
    X(Morphing,                  "VT_FEATURE_MORPHS",                       24) \
    X(SpecGloss,                 "VT_FEATURE_SPEC_GLOSS",                   25) \
    X(OrenNayar,                 "VT_FEATURE_OREN_NAYAR",                   26) \
    X(DetailNormals,             "VT_FEATURE_DETAIL_NORMALS",               27) \
    X(Displacement,              "VT_FEATURE_DISPLACEMENT",                 28) \
    X(Atmosphere,                "VT_FEATURE_ATMOSPHERE",                   29) \
    X(ShadowCatcher,             "VT_FEATURE_SHADOW_CATCHER",               30) \
    X(SkyCubemap,                "VT_FEATURE_SKY_CUBEMAP",                  31) \
    X(Instancing,                "VT_FEATURE_INSTANCING",                   32) \
    X(PlanarReflection,          "VT_FEATURE_PLANAR_REFLECTION",            33) \
    X(PlanarReflectionDepthPass, "VT_FEATURE_PLANAR_REFLECTION_DEPTH_PASS", 34) \
    X(LocalShadows,              "VT_FEATURE_LOCAL_SHADOWS",                35) \
    X(OmniShadows,               "VT_FEATURE_OMNI_SHADOWS",                 36) \
    X(DynamicBatch,              "VT_FEATURE_DYNAMIC_BATCH",                37) \
    X(PointSize,                 "VT_FEATURE_POINT_SIZE",                   38) \
    X(Unlit,                     "VT_FEATURE_UNLIT",                        39) \
    X(AreaLights,                "VT_FEATURE_AREA_LIGHTS",                  40) \
    X(VsmShadows,                "VT_FEATURE_VSM_SHADOWS",                  41) \
    X(Lightmap,                  "VT_FEATURE_LIGHTMAP",                     42) \
    X(DynamicRefraction,         "VT_FEATURE_DYNAMIC_REFRACTION",           43) \
    X(OpacityDither,             "VT_FEATURE_OPACITY_DITHER",               44) \
    X(PcssShadows,               "VT_FEATURE_PCSS_SHADOWS",                 45) \
    X(ReflectionProbe,           "VT_FEATURE_REFLECTION_PROBE",             46) \
    X(Ssr,                       "VT_FEATURE_SSR",                          47) \
    X(SurfaceLic,                "VT_FEATURE_SURFACE_LIC",                  48) \
    X(DebugPass,                 "VT_FEATURE_DEBUG_PASS",                   49) \
    X(InstancingColor,           "VT_FEATURE_INSTANCING_COLOR",             50) \
    X(LightmapBake,              "VT_FEATURE_LIGHTMAP_BAKE",                51) \
    X(LightmapBakeAccum,         "VT_FEATURE_LIGHTMAP_BAKE_ACCUM",          52) \
    X(Skybox,                    "VT_FEATURE_SKYBOX",                       62) \
    X(TransparentPass,           "VT_FEATURE_TRANSPARENT_PASS",             63)

namespace visutwin::canvas
{
    enum class ShaderFeature : uint64_t
    {
#define VT_DECLARE_SHADER_FEATURE(symbol, defineName, bit) symbol = 1ull << bit,
        VT_SHADER_FEATURES(VT_DECLARE_SHADER_FEATURE)
#undef VT_DECLARE_SHADER_FEATURE
    };

    constexpr uint64_t shaderFeatureBit(const ShaderFeature feature)
    {
        return static_cast<uint64_t>(feature);
    }
}
