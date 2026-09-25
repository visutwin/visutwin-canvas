// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.07.2026.
//
#pragma once

#include <chrono>

#include "particleSystemComponent.h"
#include "framework/applicationStats.h"
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
                engine->systems()->on("update", [engine](const float dt) {
                    // stats.particles, as upstream: emitters simulated this frame and
                    // the time it took.
                    ParticleStats* stats = engine->stats() ? &engine->stats()->particles() : nullptr;
                    const auto start = std::chrono::steady_clock::now();
                    for (auto* component : ParticleSystemComponent::instances()) {
                        // active(): a system under a disabled PARENT entity stops too.
                        if (component && component->active() && component->update(dt) && stats) {
                            stats->_updatesPerFrame++;
                        }
                    }
                    if (stats) {
                        stats->_frameTime +=
                            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
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
