// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.09.2025.
//

#pragma once

#include <algorithm>
#include <memory>
#include <vector>

#include "core/math/color.h"
#include "core/math/vector3.h"
#include "framework/components/component.h"
#include "scene/constants.h"

namespace visutwin::canvas
{
    class Light;

    /*
     * The LightComponent enables an Entity to light the scene.
     */
    class LightComponent : public Component
    {
    public:
        LightComponent(IComponentSystem* system, Entity* entity);
        ~LightComponent() override;

        void initializeComponentData() override {}

        static const std::vector<LightComponent*>& instances() { return _instances; }

        // Scene-graph Light object owned by this component. Created lazily on first access.
        Light* light() const;

        const Color& color() const { return _color; }
        void setColor(const Color& color) { _color = color; }

        float intensity() const { return _intensity; }
        void setIntensity(const float intensity) { _intensity = intensity; }

        LightType type() const { return _type; }
        void setType(const LightType type) { _type = type; }

        float range() const { return _range; }
        void setRange(const float range) { _range = range; }

        float innerConeAngle() const { return _innerConeAngle; }
        void setInnerConeAngle(const float angleDegrees) { _innerConeAngle = angleDegrees; }

        float outerConeAngle() const { return _outerConeAngle; }
        void setOuterConeAngle(const float angleDegrees) { _outerConeAngle = angleDegrees; }

        LightFalloff falloffMode() const { return _falloffMode; }
        void setFalloffMode(const LightFalloff mode) { _falloffMode = mode; }

        uint32_t mask() const { return _mask; }
        void setMask(const uint32_t value) { _mask = value; }

        bool castShadows() const { return _castShadows; }
        void setCastShadows(const bool castShadows) { _castShadows = castShadows; }

        float shadowBias() const { return _shadowBias; }
        void setShadowBias(const float value) { _shadowBias = value; }

        float shadowNormalBias() const { return _shadowNormalBias; }
        void setShadowNormalBias(const float value) { _shadowNormalBias = value; }

        float shadowStrength() const { return _shadowStrength; }
        void setShadowStrength(const float value) { _shadowStrength = value; }

        float shadowDistance() const { return _shadowDistance; }
        void setShadowDistance(const float value) { _shadowDistance = value; }

        int shadowResolution() const { return _shadowResolution; }
        void setShadowResolution(const int value) { _shadowResolution = value; }

        // Default = SHADOW_PCF3_32F (depth-comparison 3×3 PCF).
        // Set to SHADOW_VSM_16F for exponential variance shadow maps with soft
        // edges (mirrors upstream pc.SHADOW_VSM_16F).
        ShadowType shadowType() const { return _shadowType; }
        void setShadowType(const ShadowType value) { _shadowType = value; }

        // VSM-only: total kernel taps for the separable gaussian blur applied
        // to the moments texture (must be odd, ≥ 3). Larger = softer edges and
        // less wing-tip / silhouette flicker, at higher GPU cost.
        // Mirrors upstream vsmBlurSize. Default 11 = filterSize 5.
        int vsmBlurSize() const { return _vsmBlurSize; }
        void setVsmBlurSize(const int value) { _vsmBlurSize = value < 3 ? 3 : value; }

        int numCascades() const { return _numCascades; }
        void setNumCascades(const int value) { _numCascades = value; }

        float cascadeDistribution() const { return _cascadeDistribution; }
        void setCascadeDistribution(const float value) { _cascadeDistribution = value; }

        float cascadeBlend() const { return _cascadeBlend; }
        void setCascadeBlend(const float value) { _cascadeBlend = value; }

        // --- Area Light ---
        float areaWidth() const { return _areaWidth; }
        void setAreaWidth(const float value) { _areaWidth = value; }
        float areaHeight() const { return _areaHeight; }
        void setAreaHeight(const float value) { _areaHeight = value; }

        // Directional lights use the node's -Y axis as emission direction.
        Vector3 direction() const;
        Vector3 position() const;

        const std::vector<int>& layers() const { return _layers; }
        void setLayers(const std::vector<int>& layers) { _layers = layers; }
        bool rendersLayer(const int layerId) const
        {
            if (_layers.empty()) {
                return true;
            }
            return std::find(_layers.begin(), _layers.end(), layerId) != _layers.end();
        }

        /**
         *
         * Copies all light properties from a source LightComponent.
         */
        void cloneFrom(const Component* source) override;

    private:
        void syncToLight() const;

        inline static std::vector<LightComponent*> _instances;

        mutable std::unique_ptr<Light> _light;

        LightType _type = LightType::LIGHTTYPE_DIRECTIONAL;
        Color _color = Color(1.0f, 1.0f, 1.0f, 1.0f);
        float _intensity = 1.0f;
        float _range = 10.0f;
        float _innerConeAngle = 30.0f;
        float _outerConeAngle = 45.0f;
        // programmatic lights default to linear falloff.
        // GLB-loaded lights use inverse-squared.
        LightFalloff _falloffMode = LightFalloff::LIGHTFALLOFF_LINEAR;
        uint32_t _mask = MASK_AFFECT_DYNAMIC;
        bool _castShadows = false;
        float _shadowBias = 0.001f;
        float _shadowNormalBias = 0.0f;
        float _shadowStrength = 1.0f;
        float _shadowDistance = 40.0f;
        int _shadowResolution = 2048;
        ShadowType _shadowType = SHADOW_PCF3_32F;
        int _vsmBlurSize = 11;
        int _numCascades = 4;
        float _cascadeDistribution = 0.5f;
        float _cascadeBlend = 0.0f;
        float _areaWidth = 1.0f;
        float _areaHeight = 1.0f;
        std::vector<int> _layers = {LAYERID_WORLD};
    };
}
