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
    }

    void ButtonComponent::cloneFrom(const Component* source)
    {
        if (const auto* src = dynamic_cast<const ButtonComponent*>(source)) {
            _imageEntity = src->_imageEntity;
        }
    }

    void ButtonComponent::resolveClonedReferences(const Component* source, const CloneNodeMap& map)
    {
        if (const auto* src = dynamic_cast<const ButtonComponent*>(source)) {
            _imageEntity = remapCloned(src->_imageEntity, map);
        }
    }
}
