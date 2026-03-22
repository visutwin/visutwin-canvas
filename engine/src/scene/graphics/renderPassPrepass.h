// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#pragma once

#include "platform/graphics/renderPass.h"

namespace visutwin::canvas
{
    class CameraComponent;
    class Scene;
    class Renderer;

    class RenderPassPrepass : public RenderPass
    {
    public:
        RenderPassPrepass(const std::shared_ptr<GraphicsDevice>& device, Scene* scene, Renderer* renderer,
            CameraComponent* cameraComponent, Texture* sceneDepthTexture, const std::shared_ptr<RenderPassOptions>& options);

        void execute() override;
        void after() override;

    private:
        Scene* _scene = nullptr;
        Renderer* _renderer = nullptr;
        CameraComponent* _cameraComponent = nullptr;
        Texture* _sceneDepthTexture = nullptr;
    };
}

