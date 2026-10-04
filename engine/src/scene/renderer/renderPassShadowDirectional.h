// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026
//
#pragma once

#include "platform/graphics/renderPass.h"
#include "scene/camera.h"
#include "scene/light.h"

namespace visutwin::canvas
{
    class ShadowRenderer;
    class ComponentRegistry;

    /**
     * A render pass used to render directional shadows.
     *
     */
    class RenderPassShadowDirectional final : public RenderPass
    {
    public:
        RenderPassShadowDirectional(const std::shared_ptr<GraphicsDevice>& device,
            Light* light, Camera* camera, int face, ComponentRegistry* registry);

        void execute() override;
        void prepareShaders() override;

    private:
        Light* _light = nullptr;
        Camera* _camera = nullptr;
        std::shared_ptr<GraphicsDevice> _graphicsDevice;
        int _face = 0;
        // Whose casters a pass with no prepared list collects.
        ComponentRegistry* _registry = nullptr;
    };
}
