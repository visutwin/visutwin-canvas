// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2025.
//
#pragma once

#include "core/sortedLoopArray.h"
#include "scriptComponent.h"
#include "framework/components/componentSystem.h"

namespace visutwin::canvas
{
    /*
     * Allows scripts to be attached to an Entity and executed
     */
    class ScriptComponentSystem : public ComponentSystem<ScriptComponent, ScriptComponentData>
    {
    public:
        ScriptComponentSystem(Engine* engine)
            : ComponentSystem(engine, "script"),
              _components(SortedLoopArrayOptions<ScriptComponent*>{
                  .keyExtractor = [](ScriptComponent* component) {
                      return component ? static_cast<float>(component->executionOrder()) : 0.0f;
                  }
              })
        {
        }

        std::unique_ptr<Component> addComponent(Entity* entity) override
        {
            auto component = std::make_unique<ScriptComponent>(this, entity);
            component->initializeComponentData();
            component->setExecutionOrder(_executionCounter++);
            _components.append(component.get());
            return component;
        }

        void fixedUpdate(float fixedDt)
        {
            for (_components.loopIndex = 0; _components.loopIndex < static_cast<int>(_components.length); _components.loopIndex++) {
                auto* component = _components.items[_components.loopIndex];
                if (component) {
                    component->fixedUpdateScripts(fixedDt);
                }
            }
        }

        void update(float dt)
        {
            for (_components.loopIndex = 0; _components.loopIndex < static_cast<int>(_components.length); _components.loopIndex++) {
                auto* component = _components.items[_components.loopIndex];
                if (component) {
                    component->updateScripts(dt);
                }
            }
        }

        void postUpdate(float dt)
        {
            for (_components.loopIndex = 0; _components.loopIndex < static_cast<int>(_components.length); _components.loopIndex++) {
                auto* component = _components.items[_components.loopIndex];
                if (component) {
                    component->postUpdateScripts(dt);
                }
            }
        }

    private:
        SortedLoopArray<ScriptComponent*> _components;
        int _executionCounter = 0;
    };
}
