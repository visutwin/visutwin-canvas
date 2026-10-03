// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
//
#pragma once

#include <vector>

#include "platform/graphics/renderPass.h"

namespace visutwin::canvas
{
    class CameraComponent;
    class LayerComposition;
    class Scene;
    class Renderer;
    struct RenderAction;

    class RenderPassPrepass : public RenderPass
    {
    public:
        /// `actions` are the camera's render actions this frame (the camera frame's source
        /// block) and `composition` the composition they came from: the prepass draws the
        /// sublayers among them that come before the depth layer.
        RenderPassPrepass(const std::shared_ptr<GraphicsDevice>& device, Scene* scene, Renderer* renderer,
            CameraComponent* cameraComponent, Texture* sceneDepthTexture, const std::shared_ptr<RenderPassOptions>& options,
            const std::vector<RenderAction*>& actions, LayerComposition* composition);

        void execute() override;
        void after() override;
        void prepareShaders() override;

    private:
        Scene* _scene = nullptr;
        Renderer* _renderer = nullptr;
        CameraComponent* _cameraComponent = nullptr;
        Texture* _sceneDepthTexture = nullptr;
        std::vector<RenderAction*> _actions;
        LayerComposition* _composition = nullptr;
    };
}
