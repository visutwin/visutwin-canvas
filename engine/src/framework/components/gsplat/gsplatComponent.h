// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.07.2026
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
        void cloneFrom(const Component* source) override;


        void setResource(const std::shared_ptr<GSplatResource>& resource);
        const std::shared_ptr<GSplatResource>& resource() const { return _resource; }

        /// Whether the splats cast shadows for the lights that cast them (default false). A
        /// shadow draws every splat as an opaque footprint, clipped where its gaussian falls
        /// below alphaClip().
        bool castShadows() const { return _castShadows; }
        void setCastShadows(bool value);

        /// The gaussian alpha below which a splat's footprint casts no shadow (default 0.3).
        /// Higher values thin the shadow to the splats' dense cores.
        float alphaClip() const { return _alphaClip; }
        void setAlphaClip(float value);

    private:
        void applySettings();

        std::shared_ptr<GSplatResource> _resource;
        bool _castShadows = false;
        float _alphaClip = 0.3f;
        // The splat instance this component attached to the entity's render component,
        // removed again when the resource is replaced or cleared.
        MeshInstance* _meshInstance = nullptr;
    };
}
