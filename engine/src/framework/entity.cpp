// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektasuers on 18.10.2025.
//
#include "entity.h"

#include "components/componentSystem.h"
#include "components/componentSystemRegistry.h"

namespace visutwin::canvas
{
    Entity::~Entity()
    {

    }

    void Entity::onHierarchyStateChanged(const bool enabled)
    {
        // Let GraphNode update _enabledInHierarchy and handle frozen state.
        GraphNode::onHierarchyStateChanged(enabled);

        // Propagate enable/disable to components.
        //
        // A component is "active" when BOTH its own enabled flag AND the
        // entity's hierarchy enabled state are true.
        for (auto& [_, component] : _components) {
            if (!component || !component->enabled()) {
                continue;
            }

            if (enabled) {
                component->onEnable();
            } else {
                component->onDisable();
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
        for (const auto* child : children()) {
            const auto* childEntity = dynamic_cast<const Entity*>(child);
            if (childEntity) {
                auto* clonedChild = childEntity->clone();
                cloned->addChild(clonedChild);
            }
        }

        return cloned;
    }
}
