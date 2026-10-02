// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2025
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <core/eventHandler.h>

namespace visutwin::canvas
{
    class IComponentSystem;
    class Entity;
    class GraphNode;

    /// Every node of a subtree Entity::clone copied, mapped to its copy.
    using CloneNodeMap = std::unordered_map<const GraphNode*, GraphNode*>;

    using ComponentTypeID = std::size_t;

    template <class T>
    class ComponentInstanceList;

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

        /**
         * True when this component is enabled AND its entity is enabled in the
         * hierarchy — an ACTIVE component — and the exact
         * condition onEnable / onDisable fire on.
         *
         * Any loop that gathers components for a frame must test THIS, not
         * enabled(): enabled() is the component's own flag and says nothing about
         * a parent that was switched off: a light gathered by enabled() alone would
         * go on lighting the scene from a disabled entity.
         */
        [[nodiscard]] bool active() const;

        virtual void initializeComponentData() = 0;

        // Lifecycle methods.
        // Called when the component becomes active (component enabled AND entity enabled).
        virtual void onEnable() {}

        // Called when the component becomes inactive (component disabled OR entity disabled).
        virtual void onDisable() {}

        // Called after all hierarchy state changes have been processed.
        virtual void onPostStateChange() {}

        /// Relative order for enable/disable dispatch, lowest first.
        /// A rigid body returns -1 so its body exists before any
        /// sibling that might move or query it, and is torn down after them.
        /// Components with equal order keep their creation order.
        [[nodiscard]] virtual int order() const { return 0; }

        /**
         * Copy component data from a source component during Entity::clone().
         *— each system copies its properties.
         * Subclasses override to copy their specific properties.
         */
        virtual void cloneFrom(const Component* /*source*/) {}

        /**
         * Second pass of Entity::clone, run once the WHOLE subtree exists: a reference
         * the source held to a node INSIDE the cloned subtree is pointed at that node's
         * copy, and one to a node outside it is left alone. A component that holds a node
         * or entity pointer overrides this; cloneFrom copies the pointer as it is.
         */
        virtual void resolveClonedReferences(const Component* /*source*/, const CloneNodeMap& /*map*/) {}

        /// The copy of `node` if it is inside the cloned subtree, else `node` itself.
        template <class T>
        static T* remapCloned(T* node, const CloneNodeMap& map)
        {
            if (!node) {
                return nullptr;
            }
            const auto it = map.find(node);
            return it != map.end() ? static_cast<T*>(it->second) : node;
        }

    protected:
        // Called internally when the enabled setter changes the value.
        // Only fires onEnable/onDisable if entity is also enabled.
        virtual void onSetEnabled(bool oldValue, bool newValue);

        Entity* _entity;
        bool _enabled = true;

    private:
        // Which entry of its type's instance list this component is; ComponentInstanceList
        // finds the slot by it.
        template <class T>
        friend class ComponentInstanceList;
        std::uint64_t _instanceSerial = 0;

        IComponentSystem* _system;
    };
}
