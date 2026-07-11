// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "animComponentSystem.h"

#include "framework/entity.h"

namespace visutwin::canvas
{
    AnimComponentSystem::AnimComponentSystem(Engine* engine)
        : ComponentSystem(engine, "anim")
    {
        if (engine && engine->systems()) {
            engine->systems()->on("animationUpdate", [this](const float dt) {
                onAnimationUpdate(dt);
            }, this);
        }
    }

    AnimComponentSystem::~AnimComponentSystem()
    {
        if (_engine && _engine->systems()) {
            _engine->systems()->off("animationUpdate", HandleEventCallback(), this);
        }
    }

    void AnimComponentSystem::onAnimationUpdate(const float dt)
    {
        for (auto* component : AnimComponent::instances()) {
            if (!component) {
                continue;
            }

            Entity* entity = component->entity();
            if (!entity) {
                continue;
            }

            if (component->enabled() && entity->enabled()) {
                component->update(dt);
            }
        }
    }
}
