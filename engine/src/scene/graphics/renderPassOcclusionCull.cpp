// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.11.2026
//
#include "renderPassOcclusionCull.h"

#include "hiZPyramid.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/sceneMapHandles.h"
#include "scene/camera.h"
#include "scene/renderer/renderer.h"

namespace visutwin::canvas
{
    RenderPassOcclusionCull::RenderPassOcclusionCull(const std::shared_ptr<GraphicsDevice>& device, Renderer* renderer,
        Camera* camera)
        : RenderPass(device), _renderer(renderer), _camera(camera)
    {
        _name = "OcclusionCull";
    }

    void RenderPassOcclusionCull::prepareShaders()
    {
        if (_camera && _camera->renderPassHiZ()) {
            _camera->renderPassHiZ()->prepareShaders();
        }
    }

    void RenderPassOcclusionCull::execute()
    {
        const auto gd = device();
        if (!gd || !_renderer || !_camera || !_camera->renderPassHiZ()) {
            return;
        }
        // The pyramid of the depth the prepass has just written (the camera frame published
        // that texture for this frame before the prepass), built here, between device passes;
        // the camera's after-pass run of the same pass then has nothing left to do.
        _camera->renderPassHiZ()->execute();
        const auto& handle = _camera->hiZMapHandle();
        if (!handle || handle->renderVersion != gd->renderVersion()) {
            return;
        }
        // The matrix the depth was drawn with, which the camera frame published with it.
        _renderer->cullGpuInstancesInFrame(_camera, handle->viewProjection);
    }
}
