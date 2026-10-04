// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.09.2025
//
#include "component.h"

#include "componentRegistry.h"
#include "componentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"

namespace visutwin::canvas
{
    Component::Component(IComponentSystem* system, Entity* entity) : _entity(entity), _system(system)
    {
    }

    Entity* Component::entity() const
    {
        return _entity;
    }

    bool Component::active() const
    {
        // A component with no entity cannot be switched off by one; the engine's
        // own internal components are built that way.
        return enabled() && (_entity == nullptr || _entity->enabled());
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

    ComponentRegistry* Component::resolveRegistry() const
    {
        Engine* engine = _system ? _system->engine() : nullptr;
        if (!engine && _entity) {
            engine = _entity->findEngine();
        }
        return engine ? &engine->components() : nullptr;
    }

    void Component::joinRegistry(ComponentRegistry* registry)
    {
        // A type no registry lists, or one already unlisted by its destructor.
        if (!_listAdd || registry == _registry) {
            return;
        }
        if (_registry) {
            _listRemove(*_registry, this);
        }
        _registry = registry;
        if (_registry) {
            _listAdd(*_registry, this);
        }
    }

    void Component::unlistInstance()
    {
        if (_registry && _listRemove) {
            _listRemove(*_registry, this);
        }
        _registry = nullptr;
        _listAdd = nullptr;
        _listRemove = nullptr;
    }
}
