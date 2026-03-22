// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#include "renderPassDepthGrab.h"

#include "platform/graphics/graphicsDevice.h"
#include "scene/camera.h"

namespace visutwin::canvas
{
    void RenderPassDepthGrab::before()
    {
        if (!_camera) {
            return;
        }

        // depth grab publishes scene depth to a globally available slot.
        // On Metal in this port we can directly reference the depth texture from the source RT.
        const auto device = this->device();
        if (!device) {
            return;
        }

        const auto sourceTarget = _camera->renderTarget();
        Texture* sceneDepth = sourceTarget ? sourceTarget->depthBuffer() : nullptr;
        if (!sceneDepth) {
            sceneDepth = device->backBuffer() ? device->backBuffer()->depthBuffer() : nullptr;
        }
        device->setSceneDepthMap(sceneDepth);
    }

    void RenderPassDepthGrab::execute()
    {
        // DEVIATION: no copy pass needed on current Metal path because scene depth is sampled directly.
    }
}
