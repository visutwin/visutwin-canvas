// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.12.2025.
//
#pragma once

#include <memory>

#include "platform/graphics/renderPass.h"

namespace visutwin::canvas
{
    /**
     * A render pass implementing grab of a color buffer.
     */
    class RenderPassColorGrab : public RenderPass
    {
    public:
        explicit RenderPassColorGrab(const std::shared_ptr<GraphicsDevice>& device)
            : RenderPass(device) {}

        /// The device holds a raw pointer to the grab destination, so a pass that goes
        /// away while it is still published leaves every later draw binding freed memory.
        /// Destroying a grab pass is ordinary - the camera drops it when the request is
        /// withdrawn, and a camera frame drops it on any option change that rebuilds its
        /// render targets - so the pass itself has to withdraw what it published.
        ~RenderPassColorGrab();

        std::shared_ptr<RenderTarget> source() const { return _source; }

        void setSource(const std::shared_ptr<RenderTarget>& source) { _source = source; }

        void execute() override;

    private:
        // The source render target to grab the color from
        std::shared_ptr<RenderTarget> _source = nullptr;

        // The copy destination, resized with the source. Owned here rather than by
        // the backend so both backends share one allocation policy.
        std::shared_ptr<Texture> _grabTexture;
    };
}
