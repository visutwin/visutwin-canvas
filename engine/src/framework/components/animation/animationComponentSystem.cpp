// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "animationComponentSystem.h"

#include "framework/entity.h"

namespace visutwin::canvas
{
    AnimationComponentSystem::AnimationComponentSystem(Engine* engine)
        : ComponentSystem(engine, "animation")
    {
        if (engine && engine->systems()) {
            engine->systems()->on("animationUpdate", [this](const float dt) {
                onAnimationUpdate(dt);
            }, this);
        }
    }

    AnimationComponentSystem::~AnimationComponentSystem()
    {
        if (_engine && _engine->systems()) {
            _engine->systems()->off("animationUpdate", HandleEventCallback(), this);
        }
    }

    void AnimationComponentSystem::onAnimationUpdate(const float dt)
    {
        for (auto* component : AnimationComponent::instances()) {
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
