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
        /// Register a system. A duplicate id, or a second system for the same
        /// component type, is REJECTED with an error and the first one kept — it is
        /// already wired up, and overwriting the lookup maps would leave it alive,
        /// owned and still subscribed behind an id that no longer resolved to it.
        /// Fires `add` on success.
        void add(std::unique_ptr<IComponentSystem> system);

        /// Unregister and destroy a system, erasing it from the owning vector and
        /// both lookup maps together. Fires `beforeremove` while the system is still
        /// valid, then `remove`. Returns false if it was not registered here.
        bool remove(IComponentSystem* system);

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
