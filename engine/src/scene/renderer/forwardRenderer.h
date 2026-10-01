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

    private:
        // buildFrameGraph's stages, in the order it runs them.

        /// Resolves ASPECT_AUTO cameras, then culls mesh instances once per
        /// (camera, layer) the composition renders.
        void cullMeshInstancesForFrame(LayerComposition& layerComposition);
        /// Marks the lights some camera can reach (Light::visibleThisFrame).
        void cullLightsForFrame(LayerComposition& layerComposition);
        /// Culls the shadow-casting spot and omni lights and adds the passes for the
        /// ones that own their maps; returns the lights the clustered atlas shadows.
        std::vector<Light*> addLocalShadowPasses(FrameGraph* frameGraph);
        void addClusteredLightingPass(FrameGraph* frameGraph, const std::vector<Light*>& atlasLights);
        /// Fits the directional cascades for the frame's designated camera and
        /// dispatches GPU instance culling.
        void cullDirectionalShadowsAndInstances(LayerComposition& layerComposition);

        /// Groups the render actions into blocks and adds one pass per block (a
        /// forward pass, or a camera frame), plus the grab passes between them.
        void addRenderActionPasses(FrameGraph* frameGraph, LayerComposition* layerComposition);
        /// Whether the block that `action` belongs to ends with it.
        bool endsRenderActionBlock(const RenderAction& action, const RenderAction* next,
            const RenderTarget* blockTarget) const;
        void addRenderActionBlock(FrameGraph* frameGraph, LayerComposition* layerComposition,
            RenderTarget* renderTarget, int startIndex, int endIndex);
        /// The camera's persistent camera frame, pointed at this block's actions.
        void addCameraFramePass(FrameGraph* frameGraph, LayerComposition* layerComposition,
            int startIndex, int endIndex);
        void addGrabPasses(FrameGraph* frameGraph, const RenderAction& depthLayerAction);
    };
}
