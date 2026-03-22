// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis  on 01.10.2025.
//

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
        _systems[rawSystem->id()] = rawSystem;
        _systemsByComponentType[std::type_index(rawSystem->componentType())] = rawSystem;
        _ownedSystems.push_back(std::move(system));
    }

    IComponentSystem* ComponentSystemRegistry::getById(const std::string& id) const
    {
        const auto it = _systems.find(id);
        return it != _systems.end() ? it->second : nullptr;
    }
}
