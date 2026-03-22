// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.09.2025.
//
#pragma once

#include <typeindex>
#include <unordered_map>

#include "core/eventHandler.h"

namespace visutwin::canvas
{
    class IComponentSystem;

    /*
     * The ComponentSystemRegistry manages the instances of an application's ComponentSystems
     */
    class ComponentSystemRegistry : public EventHandler
    {
    public:
        void add(std::unique_ptr<IComponentSystem> system);

        IComponentSystem* getById(const std::string& id) const;

        template<typename ComponentType>
        IComponentSystem* getByComponentType() const
        {
            const auto it = _systemsByComponentType.find(std::type_index(typeid(ComponentType)));
            return it != _systemsByComponentType.end() ? it->second : nullptr;
        }

        /**
         * Look up a component system by runtime type_info.
         * Useful when the concrete Component type is only known at runtime (e.g., during clone).
         */
        IComponentSystem* getByComponentTypeInfo(const std::type_info& typeInfo) const
        {
            const auto it = _systemsByComponentType.find(std::type_index(typeInfo));
            return it != _systemsByComponentType.end() ? it->second : nullptr;
        }

    private:
        std::vector<std::unique_ptr<IComponentSystem>> _ownedSystems;
        std::unordered_map<std::string, IComponentSystem*> _systems;
        std::unordered_map<std::type_index, IComponentSystem*> _systemsByComponentType;
    };
}
