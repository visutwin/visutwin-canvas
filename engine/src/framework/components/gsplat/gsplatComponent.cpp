// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "gsplatComponent.h"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "framework/entity.h"
#include "framework/components/render/renderComponent.h"

namespace visutwin::canvas
{
    GSplatComponent::GSplatComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        _instances.push_back(this);
    }

    GSplatComponent::~GSplatComponent()
    {
        const auto it = std::find(_instances.begin(), _instances.end(), this);
        if (it != _instances.end()) {
            _instances.erase(it);
        }
    }

    void GSplatComponent::setResource(const std::shared_ptr<GSplatResource>& resource)
    {
        _resource = resource;
        if (!_resource || !_entity) {
            return;
        }

        auto meshInstance = _resource->createMeshInstance(_entity);

        // Attach to the entity's render component (created on demand) — the
        // forward renderer picks splat instances up from the transparent bucket.
        if (auto* render = _entity->findComponent<RenderComponent>()) {
            render->addMeshInstance(std::move(meshInstance));
        } else {
            auto renderComponent = std::make_unique<RenderComponent>(nullptr, _entity);
            renderComponent->addMeshInstance(std::move(meshInstance));
            _entity->addComponentInstance(std::move(renderComponent),
                componentTypeID<RenderComponent>());
        }

        spdlog::info("GSplatComponent: attached {} splats to '{}'",
            _resource->numSplats(), _entity->name());
    }
}
