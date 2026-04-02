// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.09.2025.
//
#pragma once

#include <array>

#include "camera.h"
#include "constants.h"
#include "core/math/vector4.h"
#include "platform/graphics/graphicsDevice.h"
#include "renderer/shadowMap.h"

namespace visutwin::canvas
{
    class Light;

    /**
     * @brief Per-face shadow rendering data: shadow camera, viewport, and scissor.
     * @ingroup group_scene_lighting
     */
    class LightRenderData
    {
    public:
        LightRenderData(Camera* camera, int face, Light* light);

        Light* light;

        // Camera used to cull/render the shadow map
        Camera* shadowCamera;

        Camera* camera;

        int face;

        // Viewport for the shadow rendering to the texture (x, y, width, height)
        Vector4 shadowViewport;

        // Scissor rectangle for the shadow rendering to the texture
        Vector4 shadowScissor;
    };

    /**
     * @brief Directional, point, spot, or area light with shadow mapping and cookie projection.
     * @ingroup group_scene_lighting
     *
     * Light defines the type, color, intensity, range, and shadow parameters for a light source.
     * Shadow-casting lights own LightRenderData instances (one per face for omni shadows) that
     * hold the shadow camera, viewport, and scissor used during shadow pass rendering.
     */
    class Light
    {
    public:
        Light(GraphicsDevice* graphicsDevice, bool clusteredLighting);

        bool atlasViewportAllocated() const { return _atlasViewportAllocated; }

        bool atlasSlotUpdated() const { return _atlasSlotUpdated; }

        bool enabled() const { return _enabled; }
        void setEnabled(const bool value) { _enabled = value; }

        Texture* cookie() const { return _cookie; }

        bool visibleThisFrame() const { return _visibleThisFrame; }
        void setVisibleThisFrame(const bool value) { _visibleThisFrame = value; }

        LightType type() const { return _type; }
        void setType(const LightType value) { _type = value; }

        bool castShadows() const;
        void setCastShadows(const bool value) { _castShadows = value; }

        MaskType mask() const { return _mask; }
        void setMask(const MaskType value) { _mask = value; }

        ShadowUpdateType shadowUpdateMode() const { return _shadowUpdateMode; }
        void setShadowUpdateMode(const ShadowUpdateType mode) { _shadowUpdateMode = mode; }

        int numShadowFaces() const;

        int numCascades() const;
        void setNumCascades(int value);

        float cascadeDistribution() const { return _cascadeDistribution; }
        void setCascadeDistribution(const float value) { _cascadeDistribution = value; }

        float cascadeBlend() const { return _cascadeBlend; }
        void setCascadeBlend(const float value) { _cascadeBlend = value; }

        const std::array<Vector4, 4>& cascadeViewports() const { return _cascadeViewports; }
        const std::array<float, 64>& shadowMatrixPalette() const { return _shadowMatrixPalette; }
        float* shadowMatrixPaletteData() { return _shadowMatrixPalette.data(); }
        const std::array<float, 4>& shadowCascadeDistances() const { return _shadowCascadeDistances; }
        float* shadowCascadeDistancesData() { return _shadowCascadeDistances.data(); }

        ShadowMap* shadowMap() const { return _shadowMap; }
        void setShadowMap(ShadowMap* value) { _shadowMap = value; }

        LightRenderData* getRenderData(Camera* camera, int face);

        ShadowType shadowType() const { return _shadowType; }
        void setShadowType(const ShadowType value) { _shadowType = value; }

        GraphNode* node() const { return _node; }
        void setNode(GraphNode* value) { _node = value; }

        float shadowDistance() const { return _shadowDistance; }
        void setShadowDistance(const float value) { _shadowDistance = value; }

        int shadowResolution() const { return _shadowResolution; }
        void setShadowResolution(const int value) { _shadowResolution = value; }

        //shadowBias.
        float shadowBias() const { return _shadowBias; }
        void setShadowBias(const float value) { _shadowBias = value; }

        //_normalOffsetBias.
        float normalBias() const { return _normalBias; }
        void setNormalBias(const float value) { _normalBias = value; }

        //shadowIntensity (1 = full shadow, 0 = no shadow effect).
        float shadowIntensity() const { return _shadowIntensity; }
        void setShadowIntensity(const float value) { _shadowIntensity = value; }

        // Per-light shadow VP matrix for local lights. Set during shadow camera positioning.
        const Matrix4& shadowViewProjection() const { return _shadowViewProjection; }
        void setShadowViewProjection(const Matrix4& value) { _shadowViewProjection = value; }

        // Range and cone angle — synced from LightComponent for shadow camera setup.
        float range() const { return _range; }
        void setRange(const float value) { _range = value; }

        float outerConeAngle() const { return _outerConeAngle; }
        void setOuterConeAngle(const float value) { _outerConeAngle = value; }

        GraphicsDevice* device() const { return _device; }

    private:
        GraphicsDevice* _device;

        bool _clusteredLighting;

        bool _atlasViewportAllocated = false;

        bool _atlasSlotUpdated = false;

        bool _enabled = false;

        Texture* _cookie = nullptr;

        bool _visibleThisFrame = false;

        LightType _type = LightType::LIGHTTYPE_DIRECTIONAL;

        bool _castShadows = false;

        MaskType _mask = MaskType::MASK_NONE;

        ShadowUpdateType _shadowUpdateMode = ShadowUpdateType::SHADOWUPDATE_NONE;

        //numCascades, cascadeDistribution, _cascadeBlend.
        int _numCascades = 4;
        float _cascadeDistribution = 0.5f;   // 0=linear splits, 1=logarithmic, 0.5=practical blend
        float _cascadeBlend = 0.0f;          // 0=no blend, >0=dither transition width at cascade edges

        // Viewport rects per cascade (normalized 0..1 within shadow texture).
        // Layout matches upstream directionalCascades:
        //   1 cascade: full texture
        //   2 cascades: 2×1 vertical strip
        //   4 cascades: 2×2 grid
        std::array<Vector4, 4> _cascadeViewports = {{ Vector4(0,0,0.5f,0.5f), Vector4(0,0.5f,0.5f,0.5f), Vector4(0.5f,0,0.5f,0.5f), Vector4(0.5f,0.5f,0.5f,0.5f) }};

        // Per-cascade VP matrices (viewport-scaled). 4 matrices × 16 floats.
        //_shadowMatrixPalette.
        std::array<float, 64> _shadowMatrixPalette = {};

        // Per-cascade split distances. distances[i] = far distance of cascade i.
        //_shadowCascadeDistances.
        std::array<float, 4> _shadowCascadeDistances = {};

        ShadowMap* _shadowMap = nullptr;

        std::vector<LightRenderData*> _renderData;

        ShadowType _shadowType = SHADOW_PCF3_32F;

        GraphNode* _node = nullptr;

        float _shadowDistance = 40.0f;

        int _shadowResolution = 2048;

        //_shadowBias (-0.0005 default in the upstream engine).
        float _shadowBias = -0.0005f;

        //_normalOffsetBias.
        float _normalBias = 0.0f;

        //shadowIntensity.
        float _shadowIntensity = 1.0f;

        // Computed shadow VP matrix for local lights (set during shadow camera positioning).
        Matrix4 _shadowViewProjection = Matrix4::identity();

        // Range and cone angle — synced from LightComponent for shadow camera setup.
        float _range = 10.0f;
        float _outerConeAngle = 45.0f;
    };
}
