// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "buttonComponent.h"

#include <algorithm>

#include "framework/entity.h"

namespace visutwin::canvas
{
    ButtonComponent::ButtonComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        _instances.push_back(this);
    }

    ButtonComponent::~ButtonComponent()
    {
        std::erase(_instances, this);
        // The image entity may outlive the button; its destroy event must not call
        // back into a freed component.
        if (_imageEntityDestroyed) {
            _imageEntityDestroyed->off();
        }
    }

    void ButtonComponent::cloneFrom(const Component* source)
    {
        if (const auto* src = dynamic_cast<const ButtonComponent*>(source)) {
            setImageEntity(src->_imageEntity);
        }
    }

    void ButtonComponent::resolveClonedReferences(const Component* source, const CloneNodeMap& map)
    {
        if (const auto* src = dynamic_cast<const ButtonComponent*>(source)) {
            setImageEntity(remapCloned(src->_imageEntity, map));
        }
    }

    void ButtonComponent::setImageEntity(Entity* entity)
    {
        if (_imageEntityDestroyed) {
            _imageEntityDestroyed->off();
            _imageEntityDestroyed.reset();
        }
        _imageEntity = entity;
        if (_imageEntity) {
            _imageEntityDestroyed = _imageEntity->on("destroy", [this](const EventArgs&) {
                _imageEntity = nullptr;
                _imageEntityDestroyed.reset();
            });
        }
    }
}
