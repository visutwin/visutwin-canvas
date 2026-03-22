// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "elementComponent.h"

#include <algorithm>

namespace visutwin::canvas
{
    ElementComponent::ElementComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        _instances.push_back(this);
    }

    ElementComponent::~ElementComponent()
    {
        std::erase(_instances, this);
    }
}
