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
     * The Entity is a core primitive of a VisuTwin application.
     */
    class Entity : public GraphNode
    {
    public:
        virtual ~Entity();

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
            auto it = _components.find(componentTypeID<T>());
            if (it == _components.end())
            {
                return result;
            }
            result.push_back(static_cast<T*>(it->second));
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

        // Component map for generic access
        std::unordered_map<ComponentTypeID, Component*> _components;
        std::vector<std::unique_ptr<Component>> _componentStorage;

        ScriptComponent* _script = nullptr;
    };
}
