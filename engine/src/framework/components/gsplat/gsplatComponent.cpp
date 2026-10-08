// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.07.2026
//
#include "gsplatComponent.h"

#include "framework/components/componentRegistry.h"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "framework/entity.h"
#include "framework/components/render/renderComponent.h"

namespace visutwin::canvas
{
    GSplatComponent::GSplatComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        listInstance(this);
    }

    GSplatComponent::~GSplatComponent()
    {
        unlistInstance();
    }

    void GSplatComponent::setResource(const std::shared_ptr<GSplatResource>& resource)
    {
        _resource = resource;

        // A replaced or cleared resource takes its splats with it; otherwise both
        // clouds would go on drawing.
        if (_meshInstance && _entity) {
            if (auto* render = _entity->findComponent<RenderComponent>()) {
                render->removeMeshInstance(_meshInstance);
            }
        }
        _meshInstance = nullptr;

        if (!_resource || !_entity) {
            return;
        }

        auto meshInstance = _resource->createMeshInstance(_entity);

        // Attach to the entity's render component (created on demand) — the
        // forward renderer picks splat instances up from the transparent bucket.
        if (auto* render = _entity->findComponent<RenderComponent>()) {
            _meshInstance = render->addMeshInstance(std::move(meshInstance));
        } else {
            auto renderComponent = std::make_unique<RenderComponent>(nullptr, _entity);
            _meshInstance = renderComponent->addMeshInstance(std::move(meshInstance));
            _entity->addComponentInstance(std::move(renderComponent),
                componentTypeID<RenderComponent>());
        }

        spdlog::info("GSplatComponent: attached {} splats to '{}'",
            _resource->numSplats(), _entity->name());
    }

    void GSplatComponent::cloneFrom(const Component* source)
    {
        // The resource is shared; the splat instance (and its sort) is the clone's own.
        if (const auto* src = dynamic_cast<const GSplatComponent*>(source); src && src->_resource) {
            setResource(src->_resource);
        }
    }
}
