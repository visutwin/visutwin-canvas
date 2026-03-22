// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Metal-specific uniform packing, ring-buffer allocation, and per-pass deduplication.
// Extracted from MetalGraphicsDevice for single-responsibility decomposition.
//
#pragma once

#include <cstdint>
#include <simd/simd.h>
#include <vector>
#include <Metal/Metal.hpp>

#include "platform/graphics/uniformBinder.h"

namespace visutwin::canvas
{
    class Color;
    class Material;
    class Matrix4;
    class MetalUniformRingBuffer;
    class Texture;
    class Vector3;
    struct FogParams;
    struct GpuLightData;
    struct MaterialUniforms;
    struct ShadowParams;

    /**
     * Metal implementation of uniform binding. Packs CPU-side uniform data into
     * GPU structs and manages per-pass deduplication so that identical data is
     * not re-uploaded to the ring buffer.
     */
    class MetalUniformBinder : public UniformBinder
    {
    public:
        // Use base class GpuLightUniform and LightingUniforms types.
        using UniformBinder::GpuLightUniform;
        using UniformBinder::LightingUniforms;

        // ---------------------------------------------------------------
        // Per-frame uniform setters (called by renderer before draw loop)
        // ---------------------------------------------------------------

        /// Pack transform uniforms (SceneData VP + ModelData per draw).
        void setTransformUniforms(MTL::RenderCommandEncoder* encoder,
            MetalUniformRingBuffer* transformRing,
            const Matrix4& viewProjection, const Matrix4& model);

        /// Pack lighting uniforms into the internal LightingUniforms struct.
        void setLightingUniforms(const Color& ambientColor, const std::vector<GpuLightData>& lights,
            const Vector3& cameraPosition, bool enableNormalMaps, float exposure,
            const FogParams& fogParams, const ShadowParams& shadowParams,
            int toneMapping = 0);

        /// Pack environment uniforms (skybox, env atlas) into LightingUniforms.
        void setEnvironmentUniforms(Texture* envAtlas, float skyboxIntensity, float skyboxMip,
            const Vector3& skyDomeCenter, bool isDome, Texture* skyboxCubeMap);

        /// Pack screen resolution for planar reflection screen-space UV.
        void setScreenResolution(float width, float height)
        {
            if (width > 0.0f && height > 0.0f) {
                _lightingUniforms.screenInvResolution[0] = 1.0f / width;
                _lightingUniforms.screenInvResolution[1] = 1.0f / height;
                _lightingUniforms.screenInvResolution[2] = width;
                _lightingUniforms.screenInvResolution[3] = height;
            }
        }

        /// Pack blurred planar reflection parameters.
        void setReflectionBlurParams(float intensity, float blurAmount, float fadeStrength, float angleFade,
            float fadeR, float fadeG, float fadeB)
        {
            _lightingUniforms.reflectionParams[0] = intensity;
            _lightingUniforms.reflectionParams[1] = blurAmount;
            _lightingUniforms.reflectionParams[2] = fadeStrength;
            _lightingUniforms.reflectionParams[3] = angleFade;
            _lightingUniforms.reflectionFadeColor[0] = fadeR;
            _lightingUniforms.reflectionFadeColor[1] = fadeG;
            _lightingUniforms.reflectionFadeColor[2] = fadeB;
            _lightingUniforms.reflectionFadeColor[3] = 0.0f;
        }

        /// Pack planar reflection depth pass parameters.
        void setReflectionDepthParams(float planeDistance, float heightRange)
        {
            _lightingUniforms.reflectionDepthParams[0] = planeDistance;
            _lightingUniforms.reflectionDepthParams[1] = (heightRange > 0.001f) ? heightRange : 0.001f;
            _lightingUniforms.reflectionDepthParams[2] = 0.0f;
            _lightingUniforms.reflectionDepthParams[3] = 0.0f;
        }

        /// Pack clustered lighting grid parameters into LightingUniforms.
        void setClusterParams(const float* boundsMin, const float* boundsRange,
            const float* cellsCountByBoundsSize,
            int cellsX, int cellsY, int cellsZ, int maxLightsPerCell,
            int numClusteredLights)
        {
            _lightingUniforms.clusterBoundsMin[0] = boundsMin[0];
            _lightingUniforms.clusterBoundsMin[1] = boundsMin[1];
            _lightingUniforms.clusterBoundsMin[2] = boundsMin[2];
            _lightingUniforms.clusterBoundsMin[3] = 0.0f;

            _lightingUniforms.clusterBoundsRange[0] = boundsRange[0];
            _lightingUniforms.clusterBoundsRange[1] = boundsRange[1];
            _lightingUniforms.clusterBoundsRange[2] = boundsRange[2];
            _lightingUniforms.clusterBoundsRange[3] = 0.0f;

            _lightingUniforms.clusterCellsCountByBoundsSize[0] = cellsCountByBoundsSize[0];
            _lightingUniforms.clusterCellsCountByBoundsSize[1] = cellsCountByBoundsSize[1];
            _lightingUniforms.clusterCellsCountByBoundsSize[2] = cellsCountByBoundsSize[2];
            _lightingUniforms.clusterCellsCountByBoundsSize[3] = 0.0f;

            _lightingUniforms.clusterParams[0] = static_cast<uint32_t>(cellsX);
            _lightingUniforms.clusterParams[1] = static_cast<uint32_t>(cellsY);
            _lightingUniforms.clusterParams[2] = static_cast<uint32_t>(cellsZ);
            _lightingUniforms.clusterParams[3] = static_cast<uint32_t>(maxLightsPerCell);

            _lightingUniforms.clusterParams2[0] = static_cast<uint32_t>(numClusteredLights);
            _lightingUniforms.clusterParams2[1] = 0u;
            _lightingUniforms.clusterParams2[2] = 0u;
            _lightingUniforms.clusterParams2[3] = 0u;
        }

        // ---------------------------------------------------------------
        // Per-draw uniform submission with deduplication (called from draw())
        // ---------------------------------------------------------------

        void submitPerDrawUniforms(MTL::RenderCommandEncoder* encoder,
            MetalUniformRingBuffer* uniformRing,
            const Material* currentMaterial,
            const void* uniformData,
            size_t uniformSize,
            bool hdrPass);

        // ---------------------------------------------------------------
        // UniformBinder interface
        // ---------------------------------------------------------------

        void resetPassState() override;

        [[nodiscard]] bool isMaterialChanged(const Material* mat) const override
        {
            return !_materialBoundThisPass || mat != _lastBoundMaterial;
        }

        [[nodiscard]] Texture* envAtlasTexture() const override { return _envAtlasTexture; }
        [[nodiscard]] Texture* skyboxCubeMapTexture() const override { return _skyboxCubeMapTexture; }
        [[nodiscard]] Texture* shadowTexture() const override { return _shadowTexture; }
        [[nodiscard]] Texture* localShadowTexture0() const override { return _localShadowTexture0; }
        [[nodiscard]] Texture* localShadowTexture1() const override { return _localShadowTexture1; }
        [[nodiscard]] Texture* omniShadowCube0() const override { return _omniShadowCube0; }
        [[nodiscard]] Texture* omniShadowCube1() const override { return _omniShadowCube1; }

    private:
        // Scene-global texture pointers set by setLightingUniforms / setEnvironmentUniforms,
        // read by MetalGraphicsDevice::draw() for texture binding.
        Texture* _envAtlasTexture = nullptr;
        Texture* _skyboxCubeMapTexture = nullptr;
        Texture* _shadowTexture = nullptr;
        Texture* _localShadowTexture0 = nullptr;
        Texture* _localShadowTexture1 = nullptr;
        Texture* _omniShadowCube0 = nullptr;
        Texture* _omniShadowCube1 = nullptr;

        // SceneData (VP matrix) dedup.
        bool _sceneDataBoundThisPass = false;
        simd::float4x4 _cachedSceneVP{};

        // Lighting dedup: FNV-1a hash of LightingUniforms.
        bool _lightingBoundThisPass = false;
        uint32_t _lastLightingHash = 0;
        size_t _lastLightingOffset = 0;

        // Material dedup: pointer comparison.
        bool _materialBoundThisPass = false;
        const Material* _lastBoundMaterial = nullptr;
        size_t _lastMaterialOffset = 0;
    };
}
