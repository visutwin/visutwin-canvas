// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.12.2025
//
#pragma once

#include <memory>

#include "platform/graphics/renderPass.h"

namespace visutwin::canvas
{
    class Camera;

    /**
     * A render pass implementing grab of a color buffer. The copy is published as the device's
     * scene colour map (shared by every camera) and on the camera's scene colour handle.
     */
    class RenderPassColorGrab : public RenderPass
    {
    public:
        RenderPassColorGrab(const std::shared_ptr<GraphicsDevice>& device, Camera* camera)
            : RenderPass(device), _camera(camera) {}

        /// Whether the grabbed colour is gamma-encoded (true, a camera rendering straight to its
        /// target) or linear (a camera frame's HDR scene), as the camera's handle reports it.
        void setGamma(const bool gamma) { _gamma = gamma; }

        /// The device holds a raw pointer to the grab destination, so a pass that goes
        /// away while it is still published leaves every later draw binding freed memory.
        /// Destroying a grab pass is ordinary - the camera drops it when the request is
        /// withdrawn, and a camera frame drops it on any option change that rebuilds its
        /// render targets - so the pass itself has to withdraw what it published.
        ~RenderPassColorGrab();

        std::shared_ptr<RenderTarget> source() const { return _source; }

        void setSource(const std::shared_ptr<RenderTarget>& source) { _source = source; }

        /// The copy this pass last made (null before it first ran): reallocated when the
        /// source resizes.
        Texture* texture() const { return _grabTexture.get(); }

        void execute() override;

    private:
        // The camera the grabbed colour is published for; null publishes no handle.
        Camera* _camera = nullptr;
        bool _gamma = true;

        // The source render target to grab the color from
        std::shared_ptr<RenderTarget> _source = nullptr;

        // The copy destination, resized with the source. Owned here rather than by
        // the backend so both backends share one allocation policy.
        std::shared_ptr<Texture> _grabTexture;
    };
}
