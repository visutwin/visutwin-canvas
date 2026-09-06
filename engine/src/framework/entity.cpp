// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektasuers on 18.10.2025.
//
#include <algorithm>

#include "entity.h"

#include "components/componentSystem.h"
#include "components/componentSystemRegistry.h"

namespace visutwin::canvas
{
    Entity::~Entity()
    {
        // Idempotent: an entity destroyed explicitly has already run this, and one
        // that was not still gets the defined teardown order rather than whatever
        // order its storage happens to release in.
        destroy();
    }

    std::vector<Component*> Entity::orderedComponents() const
    {
        // Creation order first — _componentStorage is a vector, where _components is
        // a hash map whose iteration order is unspecified and differs between runs.
        std::vector<Component*> ordered;
        ordered.reserve(_componentStorage.size());
        for (const auto& owned : _componentStorage) {
            if (owned) {
                ordered.push_back(owned.get());
            }
        }
        // Stable, so equal orders keep that creation order.
        std::stable_sort(ordered.begin(), ordered.end(),
            [](const Component* a, const Component* b) {
                return a->order() < b->order();
            });
        return ordered;
    }

    void Entity::destroy()
    {
        if (_destroying) {
            return;
        }
        _destroying = true;

        // Descendants first: a child component that reaches for a parent component
        // while tearing down still finds it alive.
        for (const auto& child : children()) {
            if (auto* childEntity = dynamic_cast<Entity*>(child.get())) {
                childEntity->destroy();
            }
        }

        // Disable in order, so a component that expects a rigid body during its own
        // onDisable still has one (the body is disabled last, being order -1... and
        // therefore FIRST in this list — which is why the release below runs the
        // other way round).
        for (auto* component : orderedComponents()) {
            if (component && component->enabled()) {
                component->onDisable();
            }
        }

        fire("destroy");

        // Release in the reverse of creation order, the C++ convention and the one
        // that undoes construction dependencies.
        _components.clear();
        while (!_componentStorage.empty()) {
            _componentStorage.pop_back();
        }
    }

    void Entity::onHierarchyStateChanged(const bool enabled)
    {
        // Let GraphNode update _enabledInHierarchy and handle frozen state.
        GraphNode::onHierarchyStateChanged(enabled);

        // Propagate enable/disable to components.
        //
        // A component is "active" when BOTH its own enabled flag AND the
        // entity's hierarchy enabled state are true.
        //
        // Dispatched in Component::order(), not in map order: a rigid body must be
        // enabled before any sibling that could move or query it, and disabled after
        // them. Iterating _components gave whatever order the hash produced, so the
        // guarantee held only by luck. Disable walks the reverse.
        const auto ordered = orderedComponents();
        const auto dispatch = [enabled](Component* component) {
            if (!component || !component->enabled()) {
                return;
            }
            if (enabled) {
                component->onEnable();
            } else {
                component->onDisable();
            }
        };
        if (enabled) {
            for (auto* component : ordered) {
                dispatch(component);
            }
        } else {
            for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
                dispatch(*it);
            }
        }

        // Second pass, after every component has seen the state change — this is
        // where a component wires itself to a sibling that had to exist first.
        for (auto* component : ordered) {
            if (component) {
                component->onPostStateChange();
            }
        }
    }

    Engine* Entity::findEngine() const
    {
        if (_engine) {
            return _engine;
        }
        auto* p = parent();
        while (p) {
            auto* entity = dynamic_cast<Entity*>(p);
            if (entity && entity->_engine) {
                return entity->_engine;
            }
            p = p->parent();
        }
        return nullptr;
    }

    Entity* Entity::clone() const
    {
        //
        // 1. Create new entity
        auto* cloned = new Entity();

        // Find engine: prefer direct reference, then walk hierarchy.
        // Entities from parsers (e.g., GLB) don't have _engine set directly,
        // but their ancestor (root node) does.
        auto* engine = findEngine();
        cloned->setEngine(engine);

        // 2. Copy GraphNode state (JS: GraphNode._cloneInternal)
        cloned->setName(name());
        cloned->setLocalPosition(localPosition());
        cloned->setLocalRotation(localRotation());
        cloned->setLocalScale(localScale());
        cloned->setEnabled(enabledLocal());
        // Clone is not in hierarchy yet — _enabledInHierarchy stays false until addChild.

        // 3. Clone each component via system->addComponent + cloneFrom
        //cloneComponent(this, clone) for each component
        for (const auto& [typeId, srcComponent] : _components) {
            auto* system = srcComponent->system();

            // Components created outside the system (e.g., by the GLB parser) may have
            // a null system pointer. Fall back to the engine's system registry using the
            // component's runtime type to find the correct system.
            if (!system && engine && engine->systems()) {
                system = engine->systems()->getByComponentTypeInfo(typeid(*srcComponent));
            }
            if (!system) {
                continue;
            }

            auto newComponent = system->addComponent(cloned);
            if (!newComponent) {
                continue;
            }

            // Copy data from source component
            newComponent->cloneFrom(srcComponent);

            auto* raw = newComponent.get();
            cloned->_components[typeId] = raw;
            cloned->_componentStorage.push_back(std::move(newComponent));

            if (typeId == componentTypeID<ScriptComponent>()) {
                cloned->_script = static_cast<ScriptComponent*>(raw);
            }
        }

        // 4. Recursively clone children
        // for each child, child._cloneRecursively(), clone.addChild(newChild)
        for (const auto& child : children()) {
            const auto* childEntity = dynamic_cast<const Entity*>(child.get());
            if (childEntity) {
                auto* clonedChild = childEntity->clone();
                cloned->addChild(clonedChild);
            }
        }

        return cloned;
    }
}
