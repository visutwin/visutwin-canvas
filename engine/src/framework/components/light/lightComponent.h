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
    class Texture;

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

        /**
         * Shadow depth bias as a 0..1 authoring value (upstream LightComponent scale;
         * its default is 0.05). It is remapped to the internal light bias as
         * `-0.01 * clamp(value, 0, 1)` — the negative internal convention is what makes
         * `shadowBias * -1000` a POSITIVE hardware polygon offset, i.e. one that pushes
         * casters AWAY from the light and removes acne. Passing the raw value through
         * inverted that and made a larger bias produce MORE shadow.
         */
        float shadowBias() const { return _shadowBias; }
        void setShadowBias(const float value);

        /** The remapped value handed to the internal Light (upstream light.shadowBias). */
        static float toLightShadowBias(float value)
        {
            return -0.01f * (value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value));
        }

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

        // VSM-only: bias scale for the Chebyshev minVariance floor. Larger
        // values clamp more variance noise → less flicker at thin edges, but
        // softer / more detached contact shadows. Default 0.0025 mirrors
        // upstream SHADOW_VSM_16F. Try 0.005–0.01 if wing-tip flicker
        // persists with the default.
        float vsmBias() const { return _vsmBias; }
        void setVsmBias(const float value) { _vsmBias = value < 0.0f ? 0.0f : value; }

        // PCSS penumbra: world-space light size + growth curve (SHADOW_PCSS_32F).
        float penumbraSize() const { return _penumbraSize; }
        void setPenumbraSize(const float value) { _penumbraSize = value < 0.0f ? 0.0f : value; }
        float penumbraFalloff() const { return _penumbraFalloff; }
        void setPenumbraFalloff(const float value) { _penumbraFalloff = value < 1.0f ? 1.0f : value; }

        int numCascades() const { return _numCascades; }
        void setNumCascades(const int value) { _numCascades = value; }

        float cascadeDistribution() const { return _cascadeDistribution; }
        void setCascadeDistribution(const float value) { _cascadeDistribution = value; }

        float cascadeBlend() const { return _cascadeBlend; }
        void setCascadeBlend(const float value) { _cascadeBlend = value; }

        // --- Light Cookie (upstream light.cookie / cookieIntensity / cookieChannel /
        // cookieFalloff) ---
        // A texture the light projects onto the scene, multiplying its color:
        // a 2D texture for spot lights, a cubemap for omni. Directional cookies
        // are not supported (upstream restricts them to local lights too).
        // The component does not own the texture.
        Texture* cookie() const { return _cookie; }
        void setCookie(Texture* value) { _cookie = value; }

        float cookieIntensity() const { return _cookieIntensity; }
        void setCookieIntensity(const float value) { _cookieIntensity = value; }

        CookieChannel cookieChannel() const { return _cookieChannel; }
        void setCookieChannel(const CookieChannel value) { _cookieChannel = value; }

        // Spot only: keep the cone angle falloff alongside the cookie (default,
        // upstream's too). Set false to let the cookie projection alone shape the
        // beam — the cone falloff is then skipped and the projection is clipped.
        bool cookieFalloff() const { return _cookieFalloff; }
        void setCookieFalloff(const bool value) { _cookieFalloff = value; }

        // --- Area Light ---
        float areaWidth() const { return _areaWidth; }
        void setAreaWidth(const float value) { _areaWidth = value; }
        float areaHeight() const { return _areaHeight; }
        void setAreaHeight(const float value) { _areaHeight = value; }
        AreaLightShape areaShape() const { return _areaShape; }
        void setAreaShape(const AreaLightShape value) { _areaShape = value; }

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
        float _shadowBias = 0.05f;   // upstream LightComponent default
        float _shadowNormalBias = 0.0f;
        float _shadowStrength = 1.0f;
        float _shadowDistance = 40.0f;
        int _shadowResolution = 2048;
        ShadowType _shadowType = SHADOW_PCF3_32F;
        int _vsmBlurSize = 11;
        float _penumbraSize = 1.0f;
        float _penumbraFalloff = 1.0f;
        float _vsmBias = 0.0025f;
        int _numCascades = 4;
        float _cascadeDistribution = 0.5f;
        float _cascadeBlend = 0.0f;
        Texture* _cookie = nullptr;
        float _cookieIntensity = 1.0f;
        CookieChannel _cookieChannel = CookieChannel::COOKIE_CHANNEL_RGB;
        bool _cookieFalloff = true;
        float _areaWidth = 1.0f;
        float _areaHeight = 1.0f;
        AreaLightShape _areaShape = AreaLightShape::LIGHTSHAPE_RECT;
        std::vector<int> _layers = {LAYERID_WORLD};
    };
}
