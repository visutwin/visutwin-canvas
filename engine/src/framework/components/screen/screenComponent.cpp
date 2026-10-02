// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
#include "screenComponent.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "core/math/quaternion.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/particlesystem/particleSystemComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"

namespace visutwin::canvas
{
    ScreenComponent::ScreenComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        _instanceList.add(this);
        calcProjectionMatrix();
    }

    ScreenComponent::~ScreenComponent()
    {
        _instanceList.remove(this);
        // Every element bound here loses its screen. On a copy,
        // because each one unbinds itself from this list.
        const auto elements = _elements;
        for (auto* element : elements) {
            element->onScreenRemove(this);
        }
        _elements.clear();
    }

    void ScreenComponent::initializeComponentData()
    {
        // Elements added BEFORE their
        // screen find it now; a nested screen keeps its own subtree.
        const std::function<void(GraphNode*)> bindDescendants = [&](GraphNode* node) {
            for (const auto& child : node->children()) {
                auto* entity = dynamic_cast<Entity*>(child.get());
                if (!entity) {
                    continue;
                }
                if (auto* element = entity->findComponent<ElementComponent>(); element && !element->screen()) {
                    element->updateScreen(_entity);
                }
                if (!entity->findComponent<ScreenComponent>()) {
                    bindDescendants(entity);
                }
            }
        };
        if (_entity) {
            bindDescendants(_entity);
        }
    }

    Vector2 ScreenComponent::canvasResolution() const
    {
        if (_entity && _entity->engine()) {
            const auto [w, h] = _entity->engine()->canvasSize();
            if (w > 0 && h > 0) {
                return Vector2(static_cast<float>(w), static_cast<float>(h));
            }
        }
        return _resolution;
    }

    void ScreenComponent::setResolution(const Vector2& value)
    {
        // A screen-space screen ignores the value and takes the canvas's.
        _resolution = _screenSpace ? canvasResolution() : value;
        updateScale();
        calcProjectionMatrix();
        dirtifyEntityLocal();
        fire("set:resolution", _resolution);
        notifyElementsResized();
    }

    const Vector2& ScreenComponent::referenceResolution() const
    {
        return _scaleMode == ScreenScaleMode::None ? _resolution : _referenceResolution;
    }

    void ScreenComponent::setReferenceResolution(const Vector2& value)
    {
        _referenceResolution = value;
        updateScale();
        calcProjectionMatrix();
        dirtifyEntityLocal();
        fire("set:referenceresolution", _referenceResolution);
        notifyElementsResized();
    }

    void ScreenComponent::setScaleMode(ScreenScaleMode value)
    {
        // World-space screens do not support scale modes.
        if (!_screenSpace && value != ScreenScaleMode::None) {
            value = ScreenScaleMode::None;
        }
        _scaleMode = value;
        setResolution(_resolution);   // force update
        fire("set:scalemode");
    }

    void ScreenComponent::setScaleBlend(const float value)
    {
        _scaleBlend = value;
        updateScale();
        calcProjectionMatrix();
        dirtifyEntityLocal();
        fire("set:scaleblend", _scaleBlend);
        notifyElementsResized();
    }

    void ScreenComponent::setScreenSpace(const bool value)
    {
        _screenSpace = value;
        if (_screenSpace) {
            _resolution = canvasResolution();
        }
        setResolution(_resolution);   // force update either way
        dirtifyEntityLocal();
        fire("set:screenspace", _screenSpace);
        for (auto* element : _elements) {
            element->fire("screen:set:screenspace", _screenSpace);
        }
    }

    void ScreenComponent::setPriority(const int value)
    {
        _priority = std::clamp(value, 0, 0x7F);
        syncDrawOrder();
    }

    void ScreenComponent::processDrawOrderSync()
    {
        _drawOrderDirty = false;
        int order = 1;
        const std::function<void(GraphNode*)> recurse = [&](GraphNode* node) {
            auto* entity = dynamic_cast<Entity*>(node);
            if (!entity) {
                return;
            }
            if (auto* element = entity->findComponent<ElementComponent>()) {
                element->setDrawOrder(order++);
            }
            // A particle system in the screen's hierarchy is sorted with the elements.
            if (auto* particles = entity->findComponent<ParticleSystemComponent>()) {
                particles->setDrawOrder(order++);
            }
            for (const auto& child : entity->children()) {
                recurse(child.get());
            }
        };
        if (_entity) {
            recurse(_entity);
        }
    }

    void ScreenComponent::onCanvasResize(const int width, const int height)
    {
        if (_screenSpace && (static_cast<float>(width) != _resolution.x ||
                             static_cast<float>(height) != _resolution.y)) {
            setResolution(Vector2(static_cast<float>(width), static_cast<float>(height)));
        }
    }

    void ScreenComponent::updateScale()
    {
        // Scale in log space, so that an x scale of 2 and a y scale of 0.5
        // blend to 1. With ScreenScaleMode::None the reference IS the resolution: scale 1.
        const Vector2& reference = referenceResolution();
        const float lx = std::log2((_resolution.x != 0.0f ? _resolution.x : 1.0f) / reference.x);
        const float ly = std::log2((_resolution.y != 0.0f ? _resolution.y : 1.0f) / reference.y);
        _scale = std::pow(2.0f, lx * (1.0f - _scaleBlend) + ly * _scaleBlend);
    }

    void ScreenComponent::calcProjectionMatrix()
    {
        const float w = _resolution.x / _scale;
        const float h = _resolution.y / _scale;
        _screenMatrix = Matrix4::ortho(0.0f, w, -h, 0.0f, 1.0f, -1.0f);
        if (!_screenSpace) {
            const Matrix4 scale = Matrix4::trs(Vector3(0.0f, 0.0f, 0.0f), Quaternion(),
                Vector3(0.5f * w, 0.5f * h, 1.0f));
            _screenMatrix = scale * _screenMatrix;
        }
    }

    void ScreenComponent::dirtifyEntityLocal()
    {
        // Dirty the screen entity's local transform so its elements re-sync.
        if (_entity) {
            _entity->setLocalPosition(_entity->localPosition());
        }
    }

    void ScreenComponent::notifyElementsResized()
    {
        for (auto* element : _elements) {
            element->onScreenResize(_resolution);
        }
    }

    void ScreenComponent::bindElement(ElementComponent* element)
    {
        if (element && std::find(_elements.begin(), _elements.end(), element) == _elements.end()) {
            _elements.push_back(element);
        }
    }

    void ScreenComponent::unbindElement(ElementComponent* element)
    {
        std::erase(_elements, element);
    }

    void ScreenComponent::cloneFrom(const Component* source)
    {
        const auto* src = dynamic_cast<const ScreenComponent*>(source);
        if (!src) {
            return;
        }
        // Clone order: space, mode, blend, priority, resolutions.
        setScreenSpace(src->_screenSpace);
        setScaleMode(src->_scaleMode);
        setScaleBlend(src->_scaleBlend);
        setPriority(src->_priority);
        setResolution(src->_resolution);
        setReferenceResolution(src->_referenceResolution);
    }
}
