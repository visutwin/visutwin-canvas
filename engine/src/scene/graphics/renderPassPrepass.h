// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
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
        void prepareShaders() override;

    private:
        Scene* _scene = nullptr;
        Renderer* _renderer = nullptr;
        CameraComponent* _cameraComponent = nullptr;
        Texture* _sceneDepthTexture = nullptr;
    };
}

