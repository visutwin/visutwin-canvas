// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Base interface for uniform packing, ring-buffer allocation, and per-pass deduplication.
// Backend implementations (Metal, Vulkan) provide concrete GPU submission logic.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "core/math/packedVector.h"

namespace visutwin::canvas
{
    class Material;
    class Texture;

    /**
     * Abstract base for uniform binding. Owns the GPU-side uniform struct definitions
     * (LightingUniforms, GpuLightUniform) and per-pass deduplication state.
     * Backend subclasses implement actual GPU buffer submission.
     */
    class UniformBinder
    {
    public:
        virtual ~UniformBinder() = default;

        // ---------------------------------------------------------------
        // GPU-side uniform structs (shared across all backends)
        // ---------------------------------------------------------------

        struct alignas(16) GpuLightUniform
        {
            PackedVector4f positionRange = {0.0f, 0.0f, 0.0f, 0.0f};
            PackedVector4f directionCone = {0.0f, -1.0f, 0.0f, 1.0f};
            PackedVector4f colorIntensity = {1.0f, 1.0f, 1.0f, 0.0f};
            PackedVector4f coneAngles = {1.0f, 1.0f, 0.0f, 0.0f};
            // [0]=lightType, [1]=castsShadows, [2]=falloffLinear, [3]=localShadowMapIndex
            PackedVector4u typeCastShadows = {0u, 0u, 0u, 0u};
            // Light cookie: [0]=hasCookie, [1]=cookieIndex (within the 2D or cube
            // pool selected by the light type), [2]=CookieChannel, [3]=cookieFalloff
            PackedVector4u cookieFlags = {0u, 0u, 0u, 1u};
        };

        struct alignas(16) LightingUniforms
        {
            PackedVector4f ambientColor = {0.0f, 0.0f, 0.0f, 0.0f};
            PackedVector4u lightCountAndFlags = {0u, 0u, 0u, 0u};
            PackedVector4u flagsAndPad = {0u, 0u, 0u, 0u};
            PackedVector4f cameraPositionSkyboxIntensity = {0.0f, 0.0f, 0.0f, 1.0f};
            PackedVector4f skyboxMipAndPad = {0.0f, 0.0f, 0.0f, 0.0f};
            GpuLightUniform lights[8];
            PackedVector4f fogColorDensity = {0.0f, 0.0f, 0.0f, 0.0f};
            PackedVector4f fogStartEndType = {10.0f, 100.0f, 0.0f, 0.0f};
            PackedVector4f shadowBiasNormalStrength = {0.001f, 0.0f, 1.0f, 0.0f};

            // CSM: 4 cascade VP matrices (viewport-scaled).
            float shadowMatrixPalette[64] = {};
            PackedVector4f shadowCascadeDistances = {0.0f, 0.0f, 0.0f, 0.0f};
            PackedVector4f shadowCascadeParams = {4.0f, 0.0f, 0.0f, 0.0f};

            PackedVector4f skyDomeCenter = {0.0f, 0.0f, 0.0f, 0.0f};
            PackedVector4f screenInvResolution = {0.0f, 0.0f, 0.0f, 0.0f};

            PackedVector4f reflectionParams = {1.0f, 0.0f, 1.0f, 0.5f};
            PackedVector4f reflectionFadeColor = {0.5f, 0.5f, 0.5f, 0.0f};
            PackedVector4f reflectionDepthParams = {0.0f, 10.0f, 0.0f, 0.0f};

            // Local light shadows (spot/point): up to 2 VP matrices + per-light params.
            float localShadowMatrix0[16] = {};
            float localShadowMatrix1[16] = {};
            PackedVector4f localShadowParams0 = {0.0001f, 0.0f, 1.0f, 0.0f};
            PackedVector4f localShadowParams1 = {0.0001f, 0.0f, 1.0f, 0.0f};

            // Omni cubemap shadow params.
            PackedVector4f omniShadowParams0 = {0.01f, 100.0f, 0.0001f, 0.0f};
            PackedVector4f omniShadowParams0Extra = {1.0f, 0.0f, 0.0f, 0.0f};
            PackedVector4f omniShadowParams1 = {0.01f, 100.0f, 0.0001f, 0.0f};
            PackedVector4f omniShadowParams1Extra = {1.0f, 0.0f, 0.0f, 0.0f};

            // Clustered lighting grid parameters.
            PackedVector4f clusterBoundsMin = {};
            PackedVector4f clusterBoundsRange = {};
            PackedVector4f clusterCellsCountByBoundsSize = {};
            PackedVector4u clusterParams = {};
            PackedVector4u clusterParams2 = {};

            // Ambient SH light probes: 9 premultiplied irradiance coefficients
            // (VT_FEATURE_LIGHT_PROBES; upstream AMBIENTSH basis).
            PackedVector4f ambientSH[9] = {};

            // Camera view-projection (column-major) for fragment-stage screen
            // projection (VT_FEATURE_DYNAMIC_REFRACTION grab-pass UV).
            float viewProjection[16] = {};

            // PCSS directional shadows (VT_FEATURE_PCSS_SHADOWS):
            // pcssParams = {filterSamples, blockerSamples, penumbraSize, penumbraFalloff};
            // per-cascade shadow-camera ortho radii and caster depth ranges.
            PackedVector4f pcssParams = {16.0f, 16.0f, 1.0f, 1.0f};
            PackedVector4f pcssCascadeRadii = {1.0f, 1.0f, 1.0f, 1.0f};
            PackedVector4f pcssCascadeDepthRanges = {1.0f, 1.0f, 1.0f, 1.0f};

            // Local light PCSS: [x]=searchArea UV (0=off), [y]=near, [z]=far, [w]=pad.
            PackedVector4f localShadowPcss0 = {0.0f, 0.01f, 100.0f, 0.0f};
            PackedVector4f localShadowPcss1 = {0.0f, 0.01f, 100.0f, 0.0f};

            // Light cookies (VT_FEATURE_COOKIE_2D / VT_FEATURE_COOKIE_CUBE): two
            // slots per kind, matching the local-shadow pools. Spot cookies carry a
            // world → cookie-UV projection; omni cookies carry the light's world
            // transform, whose rotation maps the light→fragment vector into cube space.
            float cookieMatrix2D0[16] = {};
            float cookieMatrix2D1[16] = {};
            float cookieMatrixCube0[16] = {};
            float cookieMatrixCube1[16] = {};
            // [x]=intensity, [y]=cookieFalloff (spot), [z]=CookieChannel, [w]=pad.
            PackedVector4f cookieParams2D0 = {1.0f, 1.0f, 0.0f, 0.0f};
            PackedVector4f cookieParams2D1 = {1.0f, 1.0f, 0.0f, 0.0f};
            PackedVector4f cookieParamsCube0 = {1.0f, 1.0f, 0.0f, 0.0f};
            PackedVector4f cookieParamsCube1 = {1.0f, 1.0f, 0.0f, 0.0f};

            // Reflection probe (box-projected cubemap): box bounds + params.
            // params = {boxProjection flag, intensity, maxMipLod, pad}.
            PackedVector4f reflectionProbeBoxMin = {-1.0f, -1.0f, -1.0f, 0.0f};
            PackedVector4f reflectionProbeBoxMax = {1.0f, 1.0f, 1.0f, 0.0f};
            PackedVector4f reflectionProbeParams = {1.0f, 1.0f, 6.0f, 0.0f};
            // Camera clip planes for SSR depth linearization: x=near, y=far.
            PackedVector4f cameraNearFar = {0.1f, 1000.0f, 0.0f, 0.0f};
        };

        // ---------------------------------------------------------------
        // Per-pass lifecycle
        // ---------------------------------------------------------------

        virtual void resetPassState() = 0;

        // ---------------------------------------------------------------
        // Queries
        // ---------------------------------------------------------------

        [[nodiscard]] virtual bool isMaterialChanged(const Material* mat) const = 0;

        [[nodiscard]] virtual Texture* envAtlasTexture() const = 0;
        [[nodiscard]] virtual Texture* skyboxCubeMapTexture() const = 0;
        [[nodiscard]] virtual Texture* reflectionProbeCubeTexture() const { return nullptr; }
        [[nodiscard]] virtual Texture* shadowTexture() const = 0;
        [[nodiscard]] virtual Texture* localShadowTexture0() const = 0;
        [[nodiscard]] virtual Texture* localShadowTexture1() const = 0;
        [[nodiscard]] virtual Texture* omniShadowCube0() const = 0;
        [[nodiscard]] virtual Texture* omniShadowCube1() const = 0;
        // Light cookies: 2D for spot lights, cubemap for omni. Two slots each.
        [[nodiscard]] virtual Texture* cookieTexture2D0() const { return nullptr; }
        [[nodiscard]] virtual Texture* cookieTexture2D1() const { return nullptr; }
        [[nodiscard]] virtual Texture* cookieTextureCube0() const { return nullptr; }
        [[nodiscard]] virtual Texture* cookieTextureCube1() const { return nullptr; }

        /// GPU-side atmosphere uniform struct (Nishita single-scattering parameters).
        /// 96 bytes (6 × float4), bound at Metal buffer slot 9 when VT_FEATURE_ATMOSPHERE is active.
        struct alignas(16) AtmosphereUniforms
        {
            PackedVector4f planetCenterAndRadius = {0.0f, 0.0f, 0.0f, 6371000.0f};
            PackedVector4f atmosphereRadiusAndSunIntensity = {6471000.0f, 22.0f, 0.9998f, 0.0f};
            PackedVector4f rayleighCoeffAndScaleHeight = {5.5e-6f, 13.0e-6f, 22.4e-6f, 8500.0f};
            PackedVector4f mieCoeffAndScaleHeight = {21.0e-6f, 1200.0f, 0.758f, 0.0f};
            PackedVector4f sunDirection = {0.0f, 1.0f, 0.0f, 0.0f};
            PackedVector4f cameraAltitudeAndParams = {0.0f, 32.0f, 8.0f, 0.0f};
        };

        /// Access the packed LightingUniforms struct (for backends to submit to GPU).
        [[nodiscard]] const LightingUniforms& lightingUniforms() const { return _lightingUniforms; }

        /// Access the packed AtmosphereUniforms struct.
        [[nodiscard]] const AtmosphereUniforms& atmosphereUniforms() const { return _atmosphereUniforms; }

    protected:
        LightingUniforms _lightingUniforms;
        AtmosphereUniforms _atmosphereUniforms;
    };

    // The uniform structs are memcpy'd to the GPU and must mirror the MSL
    // `LightingData`/`AtmosphereData` layout exactly. Lock size/alignment and a
    // few sentinel offsets so a mis-sized field (which would shift everything
    // after it and silently corrupt the shader read) fails at compile time.
    static_assert(sizeof(UniformBinder::GpuLightUniform) == 96);
    static_assert(std::is_trivially_copyable_v<UniformBinder::LightingUniforms>);
    static_assert(alignof(UniformBinder::LightingUniforms) == 16);
    static_assert(sizeof(UniformBinder::LightingUniforms) % 16 == 0);
    static_assert(offsetof(UniformBinder::LightingUniforms, lights) == 80);
    static_assert(offsetof(UniformBinder::LightingUniforms, viewProjection) ==
        offsetof(UniformBinder::LightingUniforms, ambientSH) + 9 * 16);
    static_assert(std::is_trivially_copyable_v<UniformBinder::AtmosphereUniforms>);
    static_assert(sizeof(UniformBinder::AtmosphereUniforms) == 96);
}
