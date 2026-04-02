// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2025.
//
#pragma once

#include <cstddef>
#include <memory>
#include <core/eventHandler.h>

namespace visutwin::canvas
{
    class IComponentSystem;
    class Entity;

    using ComponentTypeID = std::size_t;

    inline ComponentTypeID nextComponentTypeID() {
        static ComponentTypeID lastID = 0;
        return lastID++;
    }

    template <typename T>
    ComponentTypeID componentTypeID() {
        static ComponentTypeID id = nextComponentTypeID();
        return id;
    }

    /**
     * @brief Base class for ECS components that attach functionality to entities.
     * @ingroup group_framework_ecs
     *
     * Components are created and managed by their owning ComponentSystem<T>.
     * Each component has lifecycle hooks (onEnable, onDisable, onPostStateChange)
     * and can be cloned when an Entity is duplicated. Component lookup on Entity
     * is O(1) via a type-ID map.
     */
    class Component : public EventHandler
    {
    public:
        explicit Component(IComponentSystem* system, Entity* entity);
        virtual ~Component() = default;

        Entity* entity() const;

        IComponentSystem* system() const { return _system; }

        virtual bool enabled() const { return _enabled; }
        virtual void setEnabled(bool value);

        virtual void initializeComponentData() = 0;

        // Lifecycle methods matching upstream Component.
        // Called when the component becomes active (component enabled AND entity enabled).
        virtual void onEnable() {}

        // Called when the component becomes inactive (component disabled OR entity disabled).
        virtual void onDisable() {}

        // Called after all hierarchy state changes have been processed.
        virtual void onPostStateChange() {}

        /**
         * Copy component data from a source component during Entity::clone().
         *— each system copies its properties.
         * Subclasses override to copy their specific properties.
         */
        virtual void cloneFrom(const Component* source) {}

    protected:
        // Called internally when the enabled setter changes the value.
        // Matching Upstream: only fires onEnable/onDisable if entity is also enabled.
        virtual void onSetEnabled(bool oldValue, bool newValue);

        Entity* _entity;
        bool _enabled = true;

    private:
        IComponentSystem* _system;
    };
}
