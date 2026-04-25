// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Render pass running one direction of the separable VSM gaussian blur.
// Two passes per shadow update: source = shadow map → temp (horizontal),
// then source = temp → shadow map (vertical).
//
#pragma once

#include <memory>

#include "platform/graphics/renderPass.h"

namespace visutwin::canvas
{
    class RenderTarget;
    class Texture;

    class RenderPassVsmBlur final : public RenderPass
    {
    public:
        RenderPassVsmBlur(const std::shared_ptr<GraphicsDevice>& device,
            Texture* sourceTexture,
            const std::shared_ptr<RenderTarget>& targetRenderTarget,
            int shadowResolution,
            bool horizontal,
            int filterSize = 5);

        void execute() override;

    private:
        Texture* _sourceTexture;
        int _shadowResolution;
        bool _horizontal;
        int _filterSize;
    };
}
