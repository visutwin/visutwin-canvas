// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "buttonComponent.h"

#include <algorithm>

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
}
