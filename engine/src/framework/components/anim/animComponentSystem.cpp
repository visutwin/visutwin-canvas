// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.07.2026
//
#include "animComponentSystem.h"

#include "framework/components/componentRegistry.h"

#include "framework/applicationStats.h"
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
        ++_updateIndex;
        _evaluated = _offscreen = _intervalSkipped = 0;
        for (auto* component : instancesOf<AnimComponent>(componentRegistry())) {
            if (!component) {
                continue;
            }

            Entity* entity = component->entity();
            if (!entity) {
                continue;
            }

            if (component->enabled() && entity->enabled() && component->playing()) {
                switch (component->update(dt)) {
                case AnimUpdateResult::EVALUATED: ++_evaluated; break;
                case AnimUpdateResult::OFFSCREEN: ++_offscreen; break;
                case AnimUpdateResult::INTERVAL: ++_intervalSkipped; break;
                }
            }
        }

        if (_engine && _engine->stats()) {
            FrameStats& stats = _engine->stats()->frame();
            stats.animEvaluated = _evaluated;
            stats.animOffscreen = _offscreen;
            stats.animIntervalSkipped = _intervalSkipped;
        }
    }
}
