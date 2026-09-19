// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#include "renderPassDepthGrab.h"

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "scene/camera.h"
#include "sceneGrab.h"

namespace visutwin::canvas
{
    RenderPassDepthGrab::~RenderPassDepthGrab()
    {
        if (const auto device = this->device();
            device && _grabTexture && device->sceneDepthGrabMap() == _grabTexture.get()) {
            device->setSceneDepthGrabMap(nullptr);
        }
    }

    void RenderPassDepthGrab::before()
    {
        // Standalone only: publish the scene depth from the camera's target. Inside a
        // camera frame the frame owns that publication (its attachment or its prepass
        // texture), and this pass must not overwrite it with the back buffer's depth.
        if (_source || !_camera) {
            return;
        }
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
        // Copy the post-opaque depth into a sampleable texture for screen-space
        // reflections. No mip chain — depth cannot be averaged. The copy used to sit
        // in before(); RenderPass::render runs before() and execute() back to back for
        // a pass with no target of its own, so the timing is the same, and execute()
        // is where the colour grab does its copy too.
        const auto device = this->device();
        if (!device) {
            return;
        }
        const std::shared_ptr<RenderTarget> sourceTarget = _source
            ? _source : (_camera ? _camera->renderTarget() : nullptr);
        if (_source && !sourceTarget->depthBuffer()) {
            // A multisampled scene target keeps its depth in an internal buffer no
            // copy can reach; the frame publishes its prepass depth instead.
            return;
        }
        if (Texture* destination = ensureGrabTexture(device.get(), sourceTarget.get(),
                _grabTexture, true, false, "sceneDepthGrab")) {
            device->copyRenderTarget(sourceTarget.get(), nullptr, destination);
            device->setSceneDepthGrabMap(destination);
        }
    }
}
