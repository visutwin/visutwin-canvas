// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.07.2026.
//
#pragma once

#include "particleSystemComponent.h"
#include "framework/components/componentSystem.h"
#include "framework/engine.h"

namespace visutwin::canvas
{
    struct ParticleSystemComponentData
    {
        bool enabled = true;
    };

    class ParticleSystemComponentSystem
        : public ComponentSystem<ParticleSystemComponent, ParticleSystemComponentData>
    {
    public:
        explicit ParticleSystemComponentSystem(Engine* engine)
            : ComponentSystem(engine, "particlesystem")
        {
            if (engine && engine->systems()) {
                engine->systems()->on("update", [](const float dt) {
                    for (auto* component : ParticleSystemComponent::instances()) {
                        if (component && component->enabled() &&
                            component->entity() && component->entity()->enabled()) {
                            component->update(dt);
                        }
                    }
                }, this);
            }
        }

        ~ParticleSystemComponentSystem() override
        {
            if (_engine && _engine->systems()) {
                _engine->systems()->off("update", HandleEventCallback(), this);
            }
        }
    };
}
