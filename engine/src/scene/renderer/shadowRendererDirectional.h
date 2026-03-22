// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.10.2025.
//
#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "platform/graphics/renderPass.h"
#include "scene/camera.h"
#include "scene/frameGraph.h"
#include "scene/light.h"

namespace visutwin::canvas
{
    class RenderPassShadowDirectional;
    class Renderer;
    class ShadowRenderer;

    class ShadowRendererDirectional
    {
    public:
        ShadowRendererDirectional(const std::shared_ptr<GraphicsDevice>& device, Renderer* renderer, ShadowRenderer* shadowRenderer);

        // Sets up the shadow camera for a directional light: positions it to cover the scene
        // camera's frustum, sets orthographic projection, and snaps to texel grid for stability.
        // lines 72-201.
        void cull(Light* light, Camera* camera);

        std::shared_ptr<RenderPass> getLightRenderPass(Light* light, Camera* camera, int face, bool clearRenderTarget,
            bool allCascadesRendering);

        void buildNonClusteredRenderPasses(FrameGraph* frameGraph,
            const std::unordered_map<Camera*, std::vector<Light*>>& cameraDirShadowLights);

        // Compute per-cascade split distances using linear-logarithmic blend.
        // lines 204-216.
        static void generateSplitDistances(Light* light, float nearDist, float farDist);

    private:
        Renderer* _renderer = nullptr;
        ShadowRenderer* _shadowRenderer = nullptr;
        std::shared_ptr<GraphicsDevice> _device;
    };
}
