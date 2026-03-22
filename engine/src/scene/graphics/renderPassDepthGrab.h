// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#pragma once

#include "platform/graphics/renderPass.h"

namespace visutwin::canvas
{
    class Camera;

    /**
     * A render pass implementing grab of a depth buffer.
     * RenderPassDepthGrab behavior for forward depth-layer flow.
     */
    class RenderPassDepthGrab : public RenderPass
    {
    public:
        RenderPassDepthGrab(const std::shared_ptr<GraphicsDevice>& device, Camera* camera)
            : RenderPass(device), _camera(camera) {}

        void before() override;
        void execute() override;

    private:
        Camera* _camera = nullptr;
    };
}
