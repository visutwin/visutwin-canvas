// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.09.2025.
//
#include "component.h"

#include "framework/entity.h"

namespace visutwin::canvas
{
    Component::Component(IComponentSystem* system, Entity* entity) : _system(system), _entity(entity)
    {
    }

    Entity* Component::entity() const
    {
        return _entity;
    }

    void Component::setEnabled(const bool value)
    {
        const bool oldValue = _enabled;
        _enabled = value;
        onSetEnabled(oldValue, value);
    }

    void Component::onSetEnabled(const bool oldValue, const bool newValue)
    {
        if (oldValue != newValue) {
            if (_entity && _entity->enabled()) {
                if (newValue) {
                    onEnable();
                } else {
                    onDisable();
                }
            }
        }
    }
}