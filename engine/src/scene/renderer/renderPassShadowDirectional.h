// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#pragma once

#include "platform/graphics/renderPass.h"
#include "scene/camera.h"
#include "scene/light.h"

namespace visutwin::canvas
{
    class ShadowRenderer;

    /**
     * A render pass used to render directional shadows.
     *
     */
    class RenderPassShadowDirectional final : public RenderPass
    {
    public:
        RenderPassShadowDirectional(const std::shared_ptr<GraphicsDevice>& device,
            ShadowRenderer* shadowRenderer, Light* light, Camera* camera, Camera* shadowCamera, int face, bool allCascadesRendering);

        void execute() override;
        void after() override;

    private:
        ShadowRenderer* _shadowRenderer = nullptr;
        Light* _light = nullptr;
        Camera* _camera = nullptr;
        Camera* _shadowCamera = nullptr;
        std::shared_ptr<GraphicsDevice> _graphicsDevice;
        int _face = 0;
        bool _allCascadesRendering = true;
    };
}
