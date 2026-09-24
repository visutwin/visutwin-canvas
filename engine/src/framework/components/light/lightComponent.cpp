// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.02.2026.
//
#include "lightComponent.h"

#include <cmath>

#include "framework/engine.h"
#include "framework/entity.h"
#include "scene/light.h"

namespace visutwin::canvas
{
    LightComponent::LightComponent(IComponentSystem* system, Entity* entity) : Component(system, entity)
    {
        _instances.push_back(this);
    }

    LightComponent::~LightComponent()
    {
        const auto it = std::find(_instances.begin(), _instances.end(), this);
        if (it != _instances.end()) {
            _instances.erase(it);
        }
    }

    Light* LightComponent::light() const
    {
        if (!_light) {
            // Lazy creation. The Light allocates nothing itself — ShadowMap::create takes
            // the device directly — but it does need the device to ANSWER for one: the
            // shadow resolution is clamped to the device's texture limit and a VSM_16F
            // request falls back to PCF3 where half-float render targets are missing.
            // Built with a null device (no engine yet) it keeps whatever was authored,
            // and ShadowMap::create applies the same clamp at allocation time.
            const Entity* owner = entity();
            const Engine* engine = owner ? owner->engine() : nullptr;
            _light = std::make_unique<Light>(engine ? engine->graphicsDevice().get() : nullptr);
        }
        syncToLight();
        return _light.get();
    }

    void LightComponent::onEnable()
    {
        // Upstream's LightComponent adds its light to the layers here and removes it
        // in onDisable. This port has no per-layer light list — every consumer
        // sweeps LightComponent::instances() and tests active() — so the hooks exist
        // to keep the backing scene Light in step the moment the state changes,
        // rather than at whatever later point something calls light().
        syncToLight();
    }

    void LightComponent::onDisable()
    {
        syncToLight();
    }

    void LightComponent::syncToLight() const
    {
        if (!_light) {
            return;
        }
        _light->setType(_type);
        // active(), not enabled(): shadowRenderer, shadowRendererLocal and the
        // cookie pass all gate on Light::enabled(), so the scene Light has to know
        // about a disabled ENTITY too, not only a disabled component.
        _light->setEnabled(active());
        // visibleThisFrame is NOT set here. It used to be forced true on every sync,
        // which runs once a frame, so the two things that read it — the shadow passes
        // and the cookie pass — saw every light as visible and no light was ever
        // culled. Renderer::cullLights owns it now.
        _light->setCastShadows(_castShadows);
        _light->setMask(static_cast<MaskType>(_mask));
        _light->setShadowDistance(_shadowDistance);
        _light->setShadowResolution(_shadowResolution);
        _light->setShadowType(_shadowType);
        _light->setVsmBlurSize(_vsmBlurSize);
        _light->setVsmBias(_vsmBias);
        _light->setPenumbraSize(_penumbraSize);
        _light->setPenumbraFalloff(_penumbraFalloff);
        _light->setNumCascades(_numCascades);
        _light->setCascadeDistribution(_cascadeDistribution);
        _light->setCascadeBlend(_cascadeBlend);
        _light->setShadowBias(toLightShadowBias(_shadowBias));
        _light->setNormalBias(_shadowNormalBias);
        _light->setShadowIntensity(_shadowStrength);
        _light->setRange(_range);
        _light->setOuterConeAngle(_outerConeAngle);
        _light->setCookie(_cookie);
        _light->setCookieIntensity(_cookieIntensity);
        _light->setCookieChannel(_cookieChannel);
        _light->setCookieFalloff(_cookieFalloff);
        _light->setNode(_entity);

        // Once, not every sync: the renderer consumes a THISFRAME request by writing
        // NONE back, and a replay would undo that every frame. See the header.
        if (_shadowUpdateModePending) {
            _light->setShadowUpdateMode(_shadowUpdateMode);
            _shadowUpdateModePending = false;
        }
    }

    void LightComponent::setShadowBias(const float value)
    {
        _shadowBias = value;
        if (_light) {
            _light->setShadowBias(toLightShadowBias(value));
        }
    }

    void LightComponent::cloneFrom(const Component* source)
    {
        const auto* src = dynamic_cast<const LightComponent*>(source);
        if (!src) {
            return;
        }
        //copies all properties.
        _type = src->_type;
        _color = src->_color;
        _intensity = src->_intensity;
        _range = src->_range;
        _innerConeAngle = src->_innerConeAngle;
        _outerConeAngle = src->_outerConeAngle;
        _falloffMode = src->_falloffMode;
        _mask = src->_mask;
        _castShadows = src->_castShadows;
        _shadowBias = src->_shadowBias;
        _shadowNormalBias = src->_shadowNormalBias;
        _shadowStrength = src->_shadowStrength;
        _shadowDistance = src->_shadowDistance;
        _shadowResolution = src->_shadowResolution;
        _shadowUpdateMode = src->_shadowUpdateMode;
        _shadowUpdateModePending = true;
        _shadowType = src->_shadowType;
        _vsmBlurSize = src->_vsmBlurSize;
        _vsmBias = src->_vsmBias;
        _numCascades = src->_numCascades;
        _cascadeDistribution = src->_cascadeDistribution;
        _cascadeBlend = src->_cascadeBlend;
        _cookie = src->_cookie;
        _cookieIntensity = src->_cookieIntensity;
        _cookieChannel = src->_cookieChannel;
        _cookieFalloff = src->_cookieFalloff;
        _areaWidth = src->_areaWidth;
        _areaHeight = src->_areaHeight;
        _layers = src->_layers;
        setEnabled(src->enabled());
    }

    Vector3 LightComponent::direction() const
    {
        if (!_entity) {
            return Vector3(0.0f, -1.0f, 0.0f);
        }

        const auto& world = _entity->worldTransform();
        Vector3 up = Vector3(world.getColumn(1));
        up = up * -1.0f;

        if (up.lengthSquared() < 1e-8f) {
            return Vector3(0.0f, -1.0f, 0.0f);
        }

        return up.normalized();
    }

    Vector3 LightComponent::position() const
    {
        if (!_entity) {
            return Vector3(0.0f);
        }
        return _entity->position();
    }
}
