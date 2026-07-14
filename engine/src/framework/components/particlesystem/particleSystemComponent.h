// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.07.2026.
//
#pragma once

#include <memory>
#include <vector>

#include "framework/components/component.h"
#include "scene/particles/particleEmitter.h"

namespace visutwin::canvas
{
    /**
     * GPU particle system (upstream particle-system component subset).
     *
     * Authoring flow: mutate options() (pool size, lifetime, emitter shape,
     * velocity/gravity, scale/color/alpha graphs, color map, blending), then
     * call apply() to (re)build the emitter and attach its billboard mesh
     * instance to the entity. play()/pause()/stop()/reset() control playback;
     * the component system steps the GPU simulation each engine update.
     */
    class ParticleSystemComponent : public Component
    {
    public:
        ParticleSystemComponent(IComponentSystem* system, Entity* entity);
        ~ParticleSystemComponent() override;

        void initializeComponentData() override {}

        static const std::vector<ParticleSystemComponent*>& instances() { return _instances; }

        /// Authoring options — mutate then call apply().
        ParticleEmitterOptions& options() { return _options; }
        const ParticleEmitterOptions& options() const { return _options; }

        /// Build (or rebuild) the emitter from the current options and attach
        /// the particle mesh instance to the entity's RenderComponent.
        void apply();

        void play();
        void pause();
        /// Stop and clear the pool (restarts from scratch on the next play()).
        void stop();
        /// Restart the emission stream from time zero.
        void reset();

        [[nodiscard]] bool playing() const { return _emitter && _emitter->playing(); }
        [[nodiscard]] const std::shared_ptr<ParticleEmitter>& emitter() const { return _emitter; }

        /// Advance the GPU simulation (called by ParticleSystemComponentSystem).
        void update(float dt);

    private:
        inline static std::vector<ParticleSystemComponent*> _instances;

        ParticleEmitterOptions _options;
        std::shared_ptr<ParticleEmitter> _emitter;
        bool _meshAttached = false;
    };
}
