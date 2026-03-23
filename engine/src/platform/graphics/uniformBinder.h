// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Base interface for uniform packing, ring-buffer allocation, and per-pass deduplication.
// Backend implementations (Metal, Vulkan) provide concrete GPU submission logic.
//
#pragma once

#include <cstdint>

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
            float positionRange[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float directionCone[4] = {0.0f, -1.0f, 0.0f, 1.0f};
            float colorIntensity[4] = {1.0f, 1.0f, 1.0f, 0.0f};
            float coneAngles[4] = {1.0f, 1.0f, 0.0f, 0.0f};
            // [0]=lightType, [1]=castsShadows, [2]=falloffLinear, [3]=localShadowMapIndex
            uint32_t typeCastShadows[4] = {0u, 0u, 0u, 0u};
        };

        struct alignas(16) LightingUniforms
        {
            float ambientColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            uint32_t lightCountAndFlags[4] = {0u, 0u, 0u, 0u};
            uint32_t flagsAndPad[4] = {0u, 0u, 0u, 0u};
            float cameraPositionSkyboxIntensity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            float skyboxMipAndPad[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            GpuLightUniform lights[8];
            float fogColorDensity[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float fogStartEndType[4] = {10.0f, 100.0f, 0.0f, 0.0f};
            float shadowBiasNormalStrength[4] = {0.001f, 0.0f, 1.0f, 0.0f};

            // CSM: 4 cascade VP matrices (viewport-scaled).
            float shadowMatrixPalette[64] = {};
            float shadowCascadeDistances[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float shadowCascadeParams[4] = {4.0f, 0.0f, 0.0f, 0.0f};

            float skyDomeCenter[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float screenInvResolution[4] = {0.0f, 0.0f, 0.0f, 0.0f};

            float reflectionParams[4] = {1.0f, 0.0f, 1.0f, 0.5f};
            float reflectionFadeColor[4] = {0.5f, 0.5f, 0.5f, 0.0f};
            float reflectionDepthParams[4] = {0.0f, 10.0f, 0.0f, 0.0f};

            // Local light shadows (spot/point): up to 2 VP matrices + per-light params.
            float localShadowMatrix0[16] = {};
            float localShadowMatrix1[16] = {};
            float localShadowParams0[4] = {0.0001f, 0.0f, 1.0f, 0.0f};
            float localShadowParams1[4] = {0.0001f, 0.0f, 1.0f, 0.0f};

            // Omni cubemap shadow params.
            float omniShadowParams0[4] = {0.01f, 100.0f, 0.0001f, 0.0f};
            float omniShadowParams0Extra[4] = {1.0f, 0.0f, 0.0f, 0.0f};
            float omniShadowParams1[4] = {0.01f, 100.0f, 0.0001f, 0.0f};
            float omniShadowParams1Extra[4] = {1.0f, 0.0f, 0.0f, 0.0f};

            // Clustered lighting grid parameters.
            float clusterBoundsMin[4] = {};
            float clusterBoundsRange[4] = {};
            float clusterCellsCountByBoundsSize[4] = {};
            uint32_t clusterParams[4] = {};
            uint32_t clusterParams2[4] = {};
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
        [[nodiscard]] virtual Texture* shadowTexture() const = 0;
        [[nodiscard]] virtual Texture* localShadowTexture0() const = 0;
        [[nodiscard]] virtual Texture* localShadowTexture1() const = 0;
        [[nodiscard]] virtual Texture* omniShadowCube0() const = 0;
        [[nodiscard]] virtual Texture* omniShadowCube1() const = 0;

        /// GPU-side atmosphere uniform struct (Nishita single-scattering parameters).
        /// 96 bytes (6 × float4), bound at Metal buffer slot 9 when VT_FEATURE_ATMOSPHERE is active.
        struct alignas(16) AtmosphereUniforms
        {
            float planetCenterAndRadius[4] = {0.0f, 0.0f, 0.0f, 6371000.0f};
            float atmosphereRadiusAndSunIntensity[4] = {6471000.0f, 22.0f, 0.9998f, 0.0f};
            float rayleighCoeffAndScaleHeight[4] = {5.5e-6f, 13.0e-6f, 22.4e-6f, 8500.0f};
            float mieCoeffAndScaleHeight[4] = {21.0e-6f, 1200.0f, 0.758f, 0.0f};
            float sunDirection[4] = {0.0f, 1.0f, 0.0f, 0.0f};
            float cameraAltitudeAndParams[4] = {0.0f, 32.0f, 8.0f, 0.0f};
        };

        /// Access the packed LightingUniforms struct (for backends to submit to GPU).
        [[nodiscard]] const LightingUniforms& lightingUniforms() const { return _lightingUniforms; }

        /// Access the packed AtmosphereUniforms struct.
        [[nodiscard]] const AtmosphereUniforms& atmosphereUniforms() const { return _atmosphereUniforms; }

    protected:
        LightingUniforms _lightingUniforms;
        AtmosphereUniforms _atmosphereUniforms;
    };
}
