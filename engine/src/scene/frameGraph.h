// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#pragma once

#include <unordered_map>
#include <vector>

#include "platform/graphics/renderPass.h"

namespace visutwin::canvas
{
    /*
     * A frame graph represents a single rendering frame as a sequence of render passes.
     */
    class FrameGraph
    {
    public:
        // Reset the frame graph, clearing all render passes
        void reset();

        // Add a render pass to the frame
        void addRenderPass(const std::shared_ptr<RenderPass>& renderPass);

        // Compile the frame graph, optimizing render passes
        void compile();

        // Render all passes in the frame graph
        void render(GraphicsDevice* device);

    private:
        std::vector<std::shared_ptr<RenderPass>> _renderPasses;

        // Map used during frame graph compilation. It maps a render target to its previous occurrence
        std::unordered_map<RenderTarget*, std::shared_ptr<RenderPass>> _renderTargetMap;
    };
}
