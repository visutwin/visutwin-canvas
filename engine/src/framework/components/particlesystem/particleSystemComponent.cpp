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
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/render/renderComponent.h"
#include "scene/meshInstance.h"

namespace visutwin::canvas
{
    ParticleSystemComponent::ParticleSystemComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        _instanceList.add(this);
    }

    ParticleSystemComponent::~ParticleSystemComponent()
    {
        _instanceList.remove(this);
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

        if (_emitter && _emitter->options().mesh != _options.mesh) {
            // A mesh emitter draws its mesh, a quad emitter the quad: a new mesh needs a new
            // mesh instance, which the render component can only drop with the rest.
            auto* render = _entity->findComponent<RenderComponent>();
            if (render && render->meshInstances().size() == 1 && render->meshInstances()[0] == _meshInstance) {
                render->clearMeshInstances();
                _meshInstance = nullptr;
                _emitter.reset();
            } else {
                spdlog::warn("ParticleSystemComponent::apply: the mesh changed, but the render component "
                    "holds other mesh instances; keeping the previous particle mesh");
            }
        }
        if (_emitter) {
            _emitter->rebuild(_options);
            if (auto* render = _entity->findComponent<RenderComponent>(); render && !_options.layers.empty()) {
                render->setLayers(_options.layers);
            }
            return;
        }

        _emitter = std::make_shared<ParticleEmitter>(device, _options);

        auto meshInstance = _emitter->createMeshInstance(_entity);
        meshInstance->setDrawOrder(_drawOrder);
        auto* render = _entity->findComponent<RenderComponent>();
        if (!render) {
            auto renderComponent = std::make_unique<RenderComponent>(nullptr, _entity);
            render = renderComponent.get();
            _entity->addComponentInstance(std::move(renderComponent),
                componentTypeID<RenderComponent>());
        }
        if (!_options.layers.empty()) {
            render->setLayers(_options.layers);
        }
        _meshInstance = render->addMeshInstance(std::move(meshInstance));

        // Upstream: a system that does not auto play is built paused and hidden.
        if (!_options.autoPlay) {
            _emitter->setPlaying(false);
            _meshInstance->setVisible(false);
        }

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
            _emitter->setLoop(_options.loop);
            if (_meshInstance) {
                _meshInstance->setVisible(true);
            }
        }
    }

    void ParticleSystemComponent::pause()
    {
        if (_emitter) {
            _emitter->setPlaying(false);
        }
    }

    void ParticleSystemComponent::unpause()
    {
        if (_emitter) {
            _emitter->setPlaying(true);
        }
    }

    void ParticleSystemComponent::stop()
    {
        if (_emitter) {
            _emitter->stop();
        }
    }

    void ParticleSystemComponent::reset()
    {
        if (_emitter) {
            _emitter->reset();
        }
    }

    void ParticleSystemComponent::setDrawOrder(const int value)
    {
        _drawOrder = value;
        if (_meshInstance) {
            _meshInstance->setDrawOrder(value);
        }
    }

    bool ParticleSystemComponent::update(const float dt)
    {
        if (!_emitter || !_entity) {
            return false;
        }
        // The emitter runs a pending pre-warm even while paused, as upstream's reset does.
        const bool playing = _emitter->playing();
        const Matrix4& transform = _entity->worldTransform();
        _emitter->update(dt, transform);

        // Sorting, for the camera upstream would hand the emitter.
        // DEVIATION: upstream sorts for the camera that renders the emitter; this sorts once
        // a step, for the active camera that renders first (the lowest priority).
        if (_options.sort != ParticleSort::NONE) {
            const CameraComponent* camera = nullptr;
            for (const auto* candidate : CameraComponent::instances()) {
                if (candidate && candidate->active() && candidate->entity() &&
                    (!camera || candidate->priority() < camera->priority())) {
                    camera = candidate;
                }
            }
            if (camera) {
                _emitter->sort(camera->entity()->position(), transform);
            }
        }
        return playing;
    }

    void ParticleSystemComponent::cloneFrom(const Component* source)
    {
        const auto* src = dynamic_cast<const ParticleSystemComponent*>(source);
        if (!src) {
            return;
        }
        // Options are shared settings; the emitter (its particle state and GPU
        // buffers) is the clone's own, built only if the source had built one.
        _options = src->_options;
        _drawOrder = src->_drawOrder;
        if (src->_emitter) {
            apply();
            if (src->playing()) {
                play();
            }
        }
    }
}
