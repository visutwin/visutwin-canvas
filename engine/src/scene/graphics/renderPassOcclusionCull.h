// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.11.2026
//
// The GPU occlusion cull inside a camera's frame: right after the depth prepass, which draws
// every instance of a GPU-culled mesh (it binds no culled set), the camera's Hi-Z pyramid is
// built from that depth and the occlusion-culled instances are culled against it, with the
// view-projection the depth was drawn with (published with it), before the scene pass draws them. The cull made at graph build
// could only test last frame's pyramid, so what this frame uncovered was drawn a frame late;
// against this frame's depth nothing is. RenderPassCameraFrame adds it after the prepass when the
// camera requests a pyramid (CameraFrameOptions::hiZ), and marks the camera
// (Camera::gpuOcclusionInFrame) so the graph-build cull tests the frustum alone.
//
#pragma once

#include <memory>

#include "platform/graphics/renderPass.h"

namespace visutwin::canvas
{
    class Camera;
    class Renderer;

    class RenderPassOcclusionCull : public RenderPass
    {
    public:
        RenderPassOcclusionCull(const std::shared_ptr<GraphicsDevice>& device, Renderer* renderer, Camera* camera);

        void prepareShaders() override;
        void execute() override;

    private:
        Renderer* _renderer = nullptr;
        Camera* _camera = nullptr;
    };
}
