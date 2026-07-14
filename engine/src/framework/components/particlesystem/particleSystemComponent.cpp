// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.07.2026.
//
#include "particleSystemComponent.h"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/components/componentSystem.h"
#include "framework/components/render/renderComponent.h"

namespace visutwin::canvas
{
    ParticleSystemComponent::ParticleSystemComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        _instances.push_back(this);
    }

    ParticleSystemComponent::~ParticleSystemComponent()
    {
        const auto it = std::find(_instances.begin(), _instances.end(), this);
        if (it != _instances.end()) {
            _instances.erase(it);
        }
    }

    void ParticleSystemComponent::apply()
    {
        if (!_entity || !_entity->engine()) {
            spdlog::warn("ParticleSystemComponent::apply: no entity/engine");
            return;
        }
        const auto& device = _entity->engine()->graphicsDevice();
        if (!device) {
            return;
        }

        if (_emitter) {
            _emitter->rebuild(_options);
            return;
        }

        _emitter = std::make_shared<ParticleEmitter>(device, _options);

        auto meshInstance = _emitter->createMeshInstance(_entity);
        if (auto* render = _entity->findComponent<RenderComponent>()) {
            render->addMeshInstance(std::move(meshInstance));
        } else {
            auto renderComponent = std::make_unique<RenderComponent>(nullptr, _entity);
            renderComponent->addMeshInstance(std::move(meshInstance));
            _entity->addComponentInstance(std::move(renderComponent),
                componentTypeID<RenderComponent>());
        }
        _meshAttached = true;

        spdlog::info("ParticleSystemComponent: {} particles on '{}'",
            _emitter->numParticles(), _entity->name());
    }

    void ParticleSystemComponent::play()
    {
        if (!_emitter) {
            apply();
        }
        if (_emitter) {
            _emitter->setPlaying(true);
        }
    }

    void ParticleSystemComponent::pause()
    {
        if (_emitter) {
            _emitter->setPlaying(false);
        }
    }

    void ParticleSystemComponent::stop()
    {
        if (_emitter) {
            _emitter->setPlaying(false);
            _emitter->reset();
        }
    }

    void ParticleSystemComponent::reset()
    {
        if (_emitter) {
            _emitter->reset();
        }
    }

    void ParticleSystemComponent::update(const float dt)
    {
        if (!_emitter || !_emitter->playing() || !_entity) {
            return;
        }
        _emitter->update(dt, _entity->worldTransform());
    }
}
