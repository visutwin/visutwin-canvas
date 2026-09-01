// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Shared shader feature contract. ProgramLibrary resolves these once; Metal
// emits matching preprocessor defines and Vulkan passes the same feature set
// through specialization constants in its build-time compiled SPIR-V modules.
//
// Feature indices are assigned automatically from declaration order, so adding
// a feature is a one-line edit here and nothing else — there is no bit budget
// to account for. ShaderFeatureSet widens by one 32-bit word every 32 features,
// and both backends follow: Metal emits defines by name, and the Vulkan bundle
// generator (tools/generate_vulkan_shader_bundle.py) reads this same list to
// emit one specialization constant per word. Order is otherwise arbitrary;
// nothing persists an index across builds, so the list may be reordered freely.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

#define VT_SHADER_FEATURES(X) \
    X(BaseColorMap,              "VT_FEATURE_BASE_COLOR_MAP") \
    X(NormalMap,                 "VT_FEATURE_NORMAL_MAP") \
    X(MetallicRoughnessMap,      "VT_FEATURE_METAL_ROUGHNESS_MAP") \
    X(OcclusionMap,              "VT_FEATURE_OCCLUSION_MAP") \
    X(EmissiveMap,               "VT_FEATURE_EMISSIVE_MAP") \
    X(AlphaTest,                 "VT_FEATURE_ALPHA_TEST") \
    X(DoubleSided,               "VT_FEATURE_DOUBLE_SIDED") \
    X(Shadows,                   "VT_FEATURE_SHADOWS") \
    X(Fog,                       "VT_FEATURE_FOG") \
    X(VertexColors,              "VT_FEATURE_VERTEX_COLORS") \
    X(PointSpotAttenuation,      "VT_FEATURE_POINT_SPOT_ATTENUATION") \
    X(MultiLight,                "VT_FEATURE_MULTI_LIGHT") \
    X(EnvAtlas,                  "VT_FEATURE_ENV_ATLAS") \
    X(Parallax,                  "VT_FEATURE_PARALLAX") \
    X(Clearcoat,                 "VT_FEATURE_CLEARCOAT") \
    X(Anisotropy,                "VT_FEATURE_ANISOTROPY") \
    X(Sheen,                     "VT_FEATURE_SHEEN") \
    X(Iridescence,               "VT_FEATURE_IRIDESCENCE") \
    X(Transmission,              "VT_FEATURE_TRANSMISSION") \
    X(LightClustering,           "VT_FEATURE_LIGHT_CLUSTERING") \
    X(Ssao,                      "VT_FEATURE_SSAO") \
    X(LightProbes,               "VT_FEATURE_LIGHT_PROBES") \
    X(Skinning,                  "VT_FEATURE_SKINNING") \
    X(Morphing,                  "VT_FEATURE_MORPHS") \
    X(SpecGloss,                 "VT_FEATURE_SPEC_GLOSS") \
    X(OrenNayar,                 "VT_FEATURE_OREN_NAYAR") \
    X(DetailNormals,             "VT_FEATURE_DETAIL_NORMALS") \
    X(Displacement,              "VT_FEATURE_DISPLACEMENT") \
    X(Atmosphere,                "VT_FEATURE_ATMOSPHERE") \
    X(ShadowCatcher,             "VT_FEATURE_SHADOW_CATCHER") \
    X(SkyCubemap,                "VT_FEATURE_SKY_CUBEMAP") \
    X(Instancing,                "VT_FEATURE_INSTANCING") \
    X(PlanarReflection,          "VT_FEATURE_PLANAR_REFLECTION") \
    X(PlanarReflectionDepthPass, "VT_FEATURE_PLANAR_REFLECTION_DEPTH_PASS") \
    X(LocalShadows,              "VT_FEATURE_LOCAL_SHADOWS") \
    X(OmniShadows,               "VT_FEATURE_OMNI_SHADOWS") \
    X(DynamicBatch,              "VT_FEATURE_DYNAMIC_BATCH") \
    X(PointSize,                 "VT_FEATURE_POINT_SIZE") \
    X(Unlit,                     "VT_FEATURE_UNLIT") \
    X(AreaLights,                "VT_FEATURE_AREA_LIGHTS") \
    X(VsmShadows,                "VT_FEATURE_VSM_SHADOWS") \
    X(Lightmap,                  "VT_FEATURE_LIGHTMAP") \
    X(DynamicRefraction,         "VT_FEATURE_DYNAMIC_REFRACTION") \
    X(OpacityDither,             "VT_FEATURE_OPACITY_DITHER") \
    X(PcssShadows,               "VT_FEATURE_PCSS_SHADOWS") \
    X(ReflectionProbe,           "VT_FEATURE_REFLECTION_PROBE") \
    X(Ssr,                       "VT_FEATURE_SSR") \
    X(SurfaceLic,                "VT_FEATURE_SURFACE_LIC") \
    X(DebugPass,                 "VT_FEATURE_DEBUG_PASS") \
    X(InstancingColor,           "VT_FEATURE_INSTANCING_COLOR") \
    X(LightmapBake,              "VT_FEATURE_LIGHTMAP_BAKE") \
    X(LightmapBakeAccum,         "VT_FEATURE_LIGHTMAP_BAKE_ACCUM") \
    X(Cookie2D,                  "VT_FEATURE_COOKIE_2D") \
    X(CookieCube,                "VT_FEATURE_COOKIE_CUBE") \
    X(Skybox,                    "VT_FEATURE_SKYBOX") \
    X(TransparentPass,           "VT_FEATURE_TRANSPARENT_PASS")

namespace visutwin::canvas
{
    /// Feature identity. The value is a dense INDEX (not a bit mask) assigned
    /// from declaration order in VT_SHADER_FEATURES.
    enum class ShaderFeature : uint32_t
    {
#define VT_DECLARE_SHADER_FEATURE(symbol, defineName) symbol,
        VT_SHADER_FEATURES(VT_DECLARE_SHADER_FEATURE)
#undef VT_DECLARE_SHADER_FEATURE
        Count
    };

    inline constexpr size_t kShaderFeatureCount =
        static_cast<size_t>(ShaderFeature::Count);

    /// Number of 32-bit words needed to hold every feature. This is also the
    /// number of specialization constants the Vulkan backend binds, and it
    /// grows automatically as features are added.
    inline constexpr size_t kShaderFeatureWordCount =
        (kShaderFeatureCount + 31u) / 32u;

    /**
     * A resolved set of shader features — the backend-independent description
     * of one shader variant.
     *
     * Replaces the former fixed 64-bit mask: the storage widens with the
     * feature list instead of capping it, and cache keying goes through
     * hash() + equality rather than the raw value, so no arithmetic anywhere
     * depends on the set fitting in a machine word.
     */
    class ShaderFeatureSet
    {
    public:
        constexpr ShaderFeatureSet() = default;

        constexpr ShaderFeatureSet& set(const ShaderFeature feature, const bool enabled = true)
        {
            const auto index = static_cast<uint32_t>(feature);
            const uint32_t bit = 1u << (index & 31u);
            if (enabled) {
                _words[index >> 5u] |= bit;
            } else {
                _words[index >> 5u] &= ~bit;
            }
            return *this;
        }

        [[nodiscard]] constexpr bool test(const ShaderFeature feature) const
        {
            const auto index = static_cast<uint32_t>(feature);
            return (_words[index >> 5u] & (1u << (index & 31u))) != 0u;
        }

        /// True when every listed feature is present — the "does this variant
        /// carry the features I asked for" check used by the smoke tests.
        [[nodiscard]] constexpr bool testAll(const std::initializer_list<ShaderFeature> features) const
        {
            for (const auto feature : features) {
                if (!test(feature)) return false;
            }
            return true;
        }

        [[nodiscard]] constexpr uint32_t word(const size_t index) const { return _words[index]; }
        [[nodiscard]] constexpr const std::array<uint32_t, kShaderFeatureWordCount>& words() const { return _words; }

        constexpr bool operator==(const ShaderFeatureSet&) const = default;

        /// FNV-1a over the feature words. Used to derive a stable numeric
        /// suffix for generated entry-point names and to seed hash containers;
        /// the cache itself compares full sets, so a collision here cannot
        /// select the wrong variant.
        [[nodiscard]] constexpr uint64_t hash() const
        {
            uint64_t value = 1469598103934665603ull;
            for (const uint32_t w : _words) {
                for (int shift = 0; shift < 32; shift += 8) {
                    value ^= static_cast<uint64_t>((w >> shift) & 0xffu);
                    value *= 1099511628211ull;
                }
            }
            return value;
        }

    private:
        std::array<uint32_t, kShaderFeatureWordCount> _words{};
    };
}
