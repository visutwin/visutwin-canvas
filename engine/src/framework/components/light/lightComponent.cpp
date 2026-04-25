// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.02.2026.
//
#include "lightComponent.h"

#include <cmath>

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
            // Lazy creation — the Light needs a GraphicsDevice pointer, but we can pass nullptr
            // since the Light object here is used only for shadow camera/render-data management,
            // not for device resource allocation (ShadowMap::create takes the device directly).
            _light = std::make_unique<Light>(nullptr, false);
        }
        syncToLight();
        return _light.get();
    }

    void LightComponent::syncToLight() const
    {
        if (!_light) {
            return;
        }
        _light->setType(_type);
        _light->setEnabled(enabled());
        _light->setVisibleThisFrame(true);
        _light->setCastShadows(_castShadows);
        _light->setMask(static_cast<MaskType>(_mask));
        _light->setShadowDistance(_shadowDistance);
        _light->setShadowResolution(_shadowResolution);
        _light->setShadowType(_shadowType);
        _light->setVsmBlurSize(_vsmBlurSize);
        _light->setNumCascades(_numCascades);
        _light->setCascadeDistribution(_cascadeDistribution);
        _light->setCascadeBlend(_cascadeBlend);
        _light->setShadowBias(_shadowBias);
        _light->setNormalBias(_shadowNormalBias);
        _light->setShadowIntensity(_shadowStrength);
        _light->setRange(_range);
        _light->setOuterConeAngle(_outerConeAngle);
        _light->setNode(_entity);

        if (_castShadows) {
            _light->setShadowUpdateMode(ShadowUpdateType::SHADOWUPDATE_REALTIME);
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
        _shadowType = src->_shadowType;
        _vsmBlurSize = src->_vsmBlurSize;
        _numCascades = src->_numCascades;
        _cascadeDistribution = src->_cascadeDistribution;
        _cascadeBlend = src->_cascadeBlend;
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
