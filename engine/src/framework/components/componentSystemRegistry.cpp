// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis  on 01.10.2025.
//

#include <algorithm>

#include "spdlog/spdlog.h"

#include "componentSystemRegistry.h"
#include "framework/components/componentSystem.h"

namespace visutwin::canvas
{
    void ComponentSystemRegistry::add(std::unique_ptr<IComponentSystem> system)
    {
        if (!system) {
            return;
        }

        IComponentSystem* rawSystem = system.get();

        // Reject a duplicate rather than shadowing. Overwriting the two lookup maps
        // used to leave the FIRST system alive and owned in _ownedSystems — and
        // still subscribed to whatever engine events it registered for, so it kept
        // updating from behind an id that no longer resolved to it. Keeping the
        // first is the safer half of the choice: it is the one already wired up.
        if (const auto existing = _systems.find(rawSystem->id());
            existing != _systems.end()) {
            spdlog::error("ComponentSystemRegistry: a system with id '{}' is already "
                "registered; ignoring the duplicate", rawSystem->id());
            return;
        }
        if (const auto existing =
                _systemsByComponentType.find(std::type_index(rawSystem->componentType()));
            existing != _systemsByComponentType.end()) {
            spdlog::error("ComponentSystemRegistry: a system for component type '{}' is "
                "already registered (id '{}'); ignoring the duplicate id '{}'",
                rawSystem->componentType().name(), existing->second->id(), rawSystem->id());
            return;
        }

        _systems[rawSystem->id()] = rawSystem;
        _systemsByComponentType[std::type_index(rawSystem->componentType())] = rawSystem;
        _ownedSystems.push_back(std::move(system));
        fire("add", rawSystem);
    }

    bool ComponentSystemRegistry::remove(IComponentSystem* system)
    {
        if (!system) {
            return false;
        }

        // All three containers together, or not at all. Erasing from the maps alone
        // would leave the system alive in _ownedSystems and still subscribed —
        // exactly the shape of the bug upstream's registry.remove had, where the
        // list entry outlived the map entry.
        const auto owned = std::find_if(_ownedSystems.begin(), _ownedSystems.end(),
            [system](const std::unique_ptr<IComponentSystem>& candidate) {
                return candidate.get() == system;
            });
        if (owned == _ownedSystems.end()) {
            return false;
        }

        fire("beforeremove", system);
        _systems.erase(system->id());
        _systemsByComponentType.erase(std::type_index(system->componentType()));
        _ownedSystems.erase(owned);   // destroys the system
        fire("remove");
        return true;
    }

    IComponentSystem* ComponentSystemRegistry::getById(const std::string& id) const
    {
        const auto it = _systems.find(id);
        return it != _systems.end() ? it->second : nullptr;
    }
}
