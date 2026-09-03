// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#include "renderPassDepthGrab.h"

#include "platform/graphics/graphicsDevice.h"
#include "scene/camera.h"
#include "sceneGrab.h"

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

        // Copy the post-opaque depth into a sampleable texture for screen-space
        // reflections: sampling the still-attached depth buffer in the transparent
        // pass would be a feedback loop. No mip chain — depth cannot be averaged.
        if (Texture* destination = ensureGrabTexture(device.get(), sourceTarget.get(),
                _grabTexture, true, false, "sceneDepthGrab")) {
            device->copyRenderTarget(sourceTarget.get(), nullptr, destination);
            device->setSceneDepthGrabMap(destination);
        }
    }

    void RenderPassDepthGrab::execute()
    {
        // DEVIATION: no copy pass needed on current Metal path because scene depth is sampled directly.
    }
}
