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
        // that undoes construction dependencies — and through the SYSTEM that owns
        // each component, so a system caching its components hears `beforeremove`
        // and `remove` here exactly as it would for an explicit removal. Teardown
        // used to clear the containers directly, so a cache learned only when the
        // component's destructor got round to telling it.
        const auto ordered = orderedComponents();
        for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
            Component* component = *it;
            if (!component) {
                continue;
            }
            if (auto* system = component->system()) {
                system->removeComponent(this);
            } else if (const auto typeId = componentTypeIdOf(component)) {
                // No owning system: an entity assembled by hand, or a test probe.
                removeComponentInstance(*typeId);
            }
        }

        // Anything the loop could not place — a component whose system removed a
        // different one, say — still has to go, or it would outlive its entity.
        _components.clear();
        while (!_componentStorage.empty()) {
            _componentStorage.pop_back();
        }
    }

    std::optional<ComponentTypeID> Entity::componentTypeIdOf(const Component* component) const
    {
        for (const auto& [typeId, candidate] : _components) {
            if (candidate == component) {
                return typeId;
            }
        }
        return std::nullopt;
    }

    bool Entity::removeComponentInstance(const ComponentTypeID typeId)
    {
        const auto it = _components.find(typeId);
        if (it == _components.end()) {
            return false;
        }
        Component* component = it->second;

        // Disabled before it is unhooked, so it releases whatever onEnable acquired
        // rather than dying still holding a body, a light or a draw registration.
        // Skipped during destroy(), which has already disabled every component in
        // order — doing it again here would deliver a second onDisable, and the
        // ordered sweep is the whole reason destroy() disables before it releases.
        if (!_destroying && component && component->enabled()) {
            component->onDisable();
        }
        if (component) {
            component->fire("beforeremove");
        }

        _components.erase(it);
        if (typeId == componentTypeID<ScriptComponent>()) {
            _script = nullptr;
        }
        for (auto storage = _componentStorage.begin();
             storage != _componentStorage.end(); ++storage) {
            if (storage->get() == component) {
                _componentStorage.erase(storage);   // destroys it
                break;
            }
        }
        return true;
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
        CloneNodeMap map;
        Entity* cloned = cloneRecursively(map);
        resolveClonedReferences(*this, *cloned, map);
        return cloned;
    }

    Entity* Entity::cloneRecursively(CloneNodeMap& map) const
    {
        auto* cloned = new Entity();
        map[this] = cloned;

        // Entities from parsers (e.g., GLB) don't have _engine set directly, but their
        // ancestor (root node) does.
        auto* engine = findEngine();
        cloned->setEngine(engine);

        // GraphNode state (upstream GraphNode._cloneInternal).
        cloned->setName(name());
        cloned->tags().add(tags().list());
        cloned->setLocalPosition(localPosition());
        cloned->setLocalRotation(localRotation());
        cloned->setLocalScale(localScale());
        cloned->setEnabled(enabledLocal());
        // Not in a hierarchy yet, so nothing is enabled: adding the clone to a parent
        // is what fires onEnable, after every component has its data.

        // Components in CREATION order, as upstream walks `this.c` — the map's order is
        // a hash detail and would give each clone a different order.
        for (const auto& srcComponent : _componentStorage) {
            const Component& srcRef = *srcComponent;
            auto* system = srcRef.system();

            // Components created outside the system (e.g., by the GLB parser) may have a
            // null system pointer; find the system from the component's runtime type.
            if (!system && engine && engine->systems()) {
                system = engine->systems()->getByComponentTypeInfo(typeid(srcRef));
            }
            if (!system) {
                continue;
            }
            const auto typeId = componentTypeIdOf(srcComponent.get());
            if (!typeId) {
                continue;
            }

            // A component cloned earlier may already have created this one (a splat or
            // particle component makes the render component it draws through); clone
            // into it rather than make a second.
            Component* raw = nullptr;
            if (const auto it = cloned->_components.find(*typeId); it != cloned->_components.end()) {
                raw = it->second;
            } else if (auto newComponent = system->addComponent(cloned)) {
                raw = cloned->addComponentInstance(std::move(newComponent), *typeId);
            }
            if (!raw) {
                continue;
            }
            raw->cloneFrom(srcComponent.get());
            raw->setEnabled(srcComponent->enabled());
        }

        // Only Entity children are copied, as upstream.
        for (const auto& child : children()) {
            if (const auto* childEntity = dynamic_cast<const Entity*>(child.get())) {
                cloned->addChild(childEntity->cloneRecursively(map));
            }
        }

        return cloned;
    }

    void Entity::resolveClonedReferences(const Entity& source, Entity& clone, const CloneNodeMap& map)
    {
        for (const auto& srcComponent : source._componentStorage) {
            const auto typeId = source.componentTypeIdOf(srcComponent.get());
            if (!typeId) {
                continue;
            }
            if (const auto it = clone._components.find(*typeId); it != clone._components.end()) {
                it->second->resolveClonedReferences(srcComponent.get(), map);
            }
        }

        // Children were cloned in order, skipping the same non-Entity nodes.
        std::vector<const Entity*> sourceChildren;
        for (const auto& child : source.children()) {
            if (const auto* e = dynamic_cast<const Entity*>(child.get())) {
                sourceChildren.push_back(e);
            }
        }
        for (const auto* sourceChild : sourceChildren) {
            if (const auto it = map.find(sourceChild); it != map.end()) {
                resolveClonedReferences(*sourceChild, *static_cast<Entity*>(it->second), map);
            }
        }
    }
}
