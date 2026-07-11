// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include <memory>
#include <vector>

#include "framework/components/component.h"
#include "scene/gsplat/gsplatResource.h"

namespace visutwin::canvas
{
    /**
     * Renders a 3D Gaussian splat set (classic path). Assign a GSplatResource
     * (loaded from a 3DGS PLY) and the component attaches a splat MeshInstance —
     * with its background depth sorter — to the entity's RenderComponent.
     */
    class GSplatComponent : public Component
    {
    public:
        GSplatComponent(IComponentSystem* system, Entity* entity);
        ~GSplatComponent() override;

        void initializeComponentData() override {}

        static const std::vector<GSplatComponent*>& instances() { return _instances; }

        void setResource(const std::shared_ptr<GSplatResource>& resource);
        const std::shared_ptr<GSplatResource>& resource() const { return _resource; }

    private:
        inline static std::vector<GSplatComponent*> _instances;

        std::shared_ptr<GSplatResource> _resource;
    };
}
