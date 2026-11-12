// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026
//
#include "renderPassDepthGrab.h"

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "scene/camera.h"
#include "sceneGrab.h"
#include "spdlog/spdlog.h"

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
        // The back buffer's depth only for a camera that renders to it: a camera with a
        // target of its own whose depth is no texture has no depth to publish, and the
        // window's would be another image.
        const auto sourceTarget = _camera->renderTarget();
        Texture* sceneDepth = sourceTarget ? sourceTarget->depthBuffer()
            : (device->backBuffer() ? device->backBuffer()->depthBuffer() : nullptr);
        device->setSceneDepthMap(sceneDepth);
    }

    void RenderPassDepthGrab::execute()
    {
        // Copy the post-opaque depth into a sampleable texture for screen-space
        // reflections. No mip chain — depth cannot be averaged. The copy is in
        // execute(), as the colour grab's is; RenderPass::render runs before() and
        // execute() back to back for a pass with no target of its own, so it lands at
        // the same point either way.
        const auto device = this->device();
        if (!device) {
            return;
        }
        const std::shared_ptr<RenderTarget> sourceTarget = _source
            ? _source : (_camera ? _camera->renderTarget() : nullptr);
        if (sourceTarget && sourceTarget != device->backBuffer() && !sourceTarget->depthBuffer()) {
            // A target whose depth is an internal buffer (a multisampled scene target, or
            // one made with RenderTargetOptions::depth alone) has none a copy can reach;
            // inside a camera frame the frame publishes its prepass depth instead. Copying
            // would take the back buffer's depth, another image.
            if (!_source && !_warnedNoDepthTexture) {
                spdlog::warn("RenderPassDepthGrab: camera target '{}' has no depth texture, so no scene depth "
                    "map; give it RenderTargetOptions::depthBuffer", sourceTarget->name());
                _warnedNoDepthTexture = true;
            }
            return;
        }
        if (Texture* destination = ensureGrabTexture(device.get(), sourceTarget.get(),
                _grabTexture, true, false, "sceneDepthGrab")) {
            device->copyRenderTarget(sourceTarget.get(), nullptr, destination);
            device->setSceneDepthGrabMap(destination);
            // The camera's own record: the copy holds the depth buffer's values as they are.
            // Inside a camera frame (a source set) the frame publishes its depth texture.
            if (_camera && !_source) {
                _camera->publishSceneDepthMap(_grabTexture, device->renderVersion(), false, false, false);
            }
        }
    }
}
