// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2025.
//
#pragma once

#include <memory>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "components/componentSystemRegistry.h"
#include "components/script/scriptComponent.h"
#include "engine.h"
#include "components/component.h"
#include "scene/graphNode.h"

namespace visutwin::canvas
{
    class ScriptComponent;

    /**
     * @brief ECS entity — a GraphNode that hosts components defining its behavior.
     * @ingroup group_framework_ecs
     *
     * Entities form the leaves of the scene graph. Functionality is added by attaching
     * Component instances (Camera, Render, Light, Script, etc.) via addComponent<T>().
     * Component lookup is O(1) through an internal type-ID map.
     */
    class Entity : public GraphNode
    {
    public:
        virtual ~Entity();

        /// Tear the entity down in a DEFINED order: descendants first, then this
        /// entity's components disabled lowest-`order()` first and released in the
        /// reverse of their creation order, then a `destroy` event. Idempotent, and
        /// called by the destructor — before it existed, components died in
        /// `unordered_map` order while siblings they referenced might already be
        /// gone. It does NOT free the node: ownership stays with the parent's
        /// `unique_ptr` (or the caller's), which is what actually reclaims memory.
        void destroy();

        /// True once destroy() has begun. Component teardown can reach back into
        /// the entity, and this is how it can tell.
        [[nodiscard]] bool destroying() const { return _destroying; }

        /// Components sorted for lifecycle dispatch: by `Component::order()`, ties
        /// broken by creation order. Enable and disable walk this rather than the
        /// component map, whose iteration order is a hash detail and varies run to
        /// run.
        [[nodiscard]] std::vector<Component*> orderedComponents() const;

        Component* addComponentInstance(std::unique_ptr<Component> component, ComponentTypeID typeId)
        {
            if (!component) {
                return nullptr;
            }
            if (const auto it = _components.find(typeId); it != _components.end()) {
                return it->second;
            }

            auto* raw = component.get();
            _components[typeId] = raw;
            _componentStorage.push_back(std::move(component));

            if (typeId == componentTypeID<ScriptComponent>()) {
                _script = static_cast<ScriptComponent*>(raw);
            }

            return raw;
        }

        template <class ComponentType>
        Component* addComponent()
        {
            const ComponentTypeID typeId = componentTypeID<ComponentType>();
            if (const auto it = _components.find(typeId); it != _components.end()) {
                return it->second;
            }

            if (!_engine || !_engine->systems()) {
                return nullptr;
            }

            auto* system = _engine->systems()->template getByComponentType<ComponentType>();
            if (!system) {
                return nullptr;
            }

            auto component = system->addComponent(this);
            if (!component) {
                return nullptr;
            }

            auto* raw = component.get();
            _components[typeId] = raw;
            _componentStorage.push_back(std::move(component));

            if constexpr (std::is_same_v<ComponentType, ScriptComponent>) {
                _script = static_cast<ScriptComponent*>(raw);
            }

            return raw;
        }

        /**
         * Get the component of the specified type from this entity.
         * Returns nullptr if the entity does not have a component of that type.
         */
        template <class T>
        requires std::derived_from<T, Component>
        T* findComponent()
        {
            auto it = _components.find(componentTypeID<T>());
            if (it == _components.end()) {
                return nullptr;
            }
            return static_cast<T*>(it->second);
        }

        /**
         * Search the entity and all of its descendants for all components of specified type.
         */
        template <class T>
        requires std::derived_from<T, Component>
        std::vector<T*> findComponents()
        {
            std::vector<T*> result;

            const auto collect = [&result](auto&& self, GraphNode* node) -> void {
                if (auto* entity = dynamic_cast<Entity*>(node)) {
                    if (auto* component = entity->template findComponent<T>()) {
                        result.push_back(component);
                    }
                }

                for (const auto& child : node->children()) {
                    self(self, child.get());
                }
            };

            collect(collect, this);
            return result;
        }

        ScriptComponent* script() const { return _script; }

        Engine* engine() const { return _engine; }
        void setEngine(Engine* engine) { _engine = engine; }

        /**
         * Called when the entity's enabled-in-hierarchy state changes.
         * Propagates onEnable/onDisable to components.
         *
         */
        void onHierarchyStateChanged(bool enabled) override;

        /**
         * Create a deep clone of the entity. Creates a new entity with the same
         * transform, components, and children hierarchy. Component data is cloned
         * via Component::cloneFrom(). The clone is NOT automatically added to any parent.
         *
         * / _cloneRecursively().
         */
        Entity* clone() const;

        /**
         * Access the component type map (for iteration during clone).
         */
        const std::unordered_map<ComponentTypeID, Component*>& components() const { return _components; }

        /**
         * Find the Engine reference by walking up the hierarchy.
         * Entities created by parsers (e.g., GLB) may not have _engine set directly,
         * but their ancestor (typically the root) does.
         */
        Engine* findEngine() const;

    private:
        Engine* _engine = nullptr;

        bool _destroying = false;

        // Component map for generic access
        std::unordered_map<ComponentTypeID, Component*> _components;
        std::vector<std::unique_ptr<Component>> _componentStorage;

        ScriptComponent* _script = nullptr;
    };
}
