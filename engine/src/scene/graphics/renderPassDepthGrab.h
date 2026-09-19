// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#pragma once

#include <memory>

#include "platform/graphics/renderPass.h"

namespace visutwin::canvas
{
    class Camera;
    class RenderTarget;

    /**
     * Copies the post-opaque scene DEPTH into a sampleable texture and publishes it
     * as the device's sceneDepthGrabMap — what screen-space reflections march
     * against, since sampling the still-attached depth buffer from the transparent
     * pass would be a feedback loop. Two owners:
     *   - standalone, built by the Camera for the depth-layer flow: the source is the
     *     camera's render target (or the back buffer) and before() also publishes the
     *     scene depth map from that target;
     *   - inside a RenderPassCameraFrame, with setSource(): the source is the frame's
     *     offscreen scene target and the frame publishes the scene depth itself, so this
     *     pass only produces the copy. Until 2026-09-19 a camera frame had no depth
     *     grab at all, so SSR under any post-processing read an unbound texture.
     */
    class RenderPassDepthGrab : public RenderPass
    {
    public:
        RenderPassDepthGrab(const std::shared_ptr<GraphicsDevice>& device, Camera* camera)
            : RenderPass(device), _camera(camera) {}

        /// Copy from this render target instead of the camera's (camera-frame mode).
        void setSource(const std::shared_ptr<RenderTarget>& source) { _source = source; }

        /// Withdraws the published depth grab for the reason RenderPassColorGrab's
        /// destructor gives: the device keeps a raw pointer to a texture this pass owns.
        ~RenderPassDepthGrab();

        void before() override;
        void execute() override;

    private:
        Camera* _camera = nullptr;
        std::shared_ptr<RenderTarget> _source;

        // The copy destination, resized with the source (see RenderPassColorGrab).
        std::shared_ptr<Texture> _grabTexture;
    };
}
