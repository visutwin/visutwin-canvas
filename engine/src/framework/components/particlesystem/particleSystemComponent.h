// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.07.2026
//
#pragma once

#include <memory>
#include <vector>

#include "framework/components/component.h"
#include "scene/particles/particleEmitter.h"

namespace visutwin::canvas
{
    /**
     * GPU particle system.
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
        void cloneFrom(const Component* source) override;


        /// Authoring options — mutate then call apply().
        ParticleEmitterOptions& options() { return _options; }
        const ParticleEmitterOptions& options() const { return _options; }

        /// Build (or rebuild) the emitter from the current options and attach
        /// the particle mesh instance to the entity's RenderComponent.
        void apply();

        /// Unfreeze, show, and loop again if the options loop.
        void play();
        /// Freeze the simulation.
        void pause();
        void unpause();
        /// Stop emitting and let the live particles finish their lives.
        void stop();
        /// Restart the emission stream from time zero (and pre-warm it if asked to).
        void reset();

        /// The order among UI draws, which a screen assigns to a
        /// particle system in its hierarchy (ScreenComponent::syncDrawOrder).
        [[nodiscard]] int drawOrder() const { return _drawOrder; }
        void setDrawOrder(int value);

        [[nodiscard]] bool playing() const { return _emitter && _emitter->playing(); }
        [[nodiscard]] const std::shared_ptr<ParticleEmitter>& emitter() const { return _emitter; }

        /// Advance the GPU simulation (called by ParticleSystemComponentSystem); false
        /// when there was nothing to simulate, which stats.particles does not count.
        bool update(float dt);

    private:

        ParticleEmitterOptions _options;
        std::shared_ptr<ParticleEmitter> _emitter;
        MeshInstance* _meshInstance = nullptr;
        int _drawOrder = 0;
    };
}
