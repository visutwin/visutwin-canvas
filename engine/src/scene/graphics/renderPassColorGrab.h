// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.12.2025.
//
#pragma once

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

        std::shared_ptr<RenderTarget> source() const { return _source; }

        void setSource(const std::shared_ptr<RenderTarget>& source) { _source = source; }

    private:
        // The source render target to grab the color from
        std::shared_ptr<RenderTarget> _source = nullptr;
    };
}
