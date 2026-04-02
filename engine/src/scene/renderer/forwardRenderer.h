// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#pragma once

#include "renderer.h"
#include "../frameGraph.h"
#include "../composition/layerComposition.h"

namespace visutwin::canvas
{
    /**
     * @brief Forward PBR renderer that builds a FrameGraph from the scene's layer composition.
     * @ingroup group_scene_renderer
     *
     * ForwardRenderer is the main rendering path. It constructs a FrameGraph containing
     * shadow passes, the main forward pass (multi-light PBR), clustered-lighting updates,
     * and post-processing (SSAO, TAA, DOF, compose). The frame graph is rebuilt each frame
     * via buildFrameGraph() and then compiled and executed.
     */
    class ForwardRenderer : public Renderer
    {
    public:
        ForwardRenderer(const std::shared_ptr<GraphicsDevice>& device, const std::shared_ptr<Scene>& scene) : Renderer(device, scene) {}

        // Builds a frame graph for the rendering of the whole frame
        void buildFrameGraph(FrameGraph* frameGraph, LayerComposition* layerComposition);

        // Adds main render pass to frame graph.
        void addMainRenderPass(FrameGraph* frameGraph, LayerComposition* layerComposition, RenderTarget* renderTarget,
            int startIndex, int endIndex);
    };
}
