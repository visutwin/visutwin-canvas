// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#pragma once

#include <scene/frameGraph.h>

#include "shadowRenderer.h"
#include "shadowMap.h"
#include "scene/camera.h"
#include "scene/light.h"

namespace visutwin::canvas
{
    class ShadowRendererLocal
    {
    public:
        ShadowRendererLocal(Renderer* renderer, ShadowRenderer* shadowRenderer) : _renderer(renderer), _shadowRenderer(shadowRenderer) {}

        Camera* prepareLights(std::vector<Light*>& shadowLights, const std::vector<Light*>& lights);

        std::vector<Light*>& shadowLights() { return _shadowLights; }

        // Prepare render passes for rendering of shadows for local non-clustered lights.
        // Each shadow face is a separate render pass as it renders to a separate render target.
        void buildNonClusteredRenderPasses(FrameGraph* frameGraph, const std::vector<Light*>& localLights);

        // Position shadow cameras for local (spot/point) lights and allocate shadow maps.
        // Mirrors Renderer::cullShadowmaps() but for local lights.
        void cullLocalLights(const std::vector<Light*>& localLights,
            const std::shared_ptr<GraphicsDevice>& device,
            std::vector<std::unique_ptr<ShadowMap>>& ownedShadowMaps);

    private:
        Renderer* _renderer;
        ShadowRenderer* _shadowRenderer;

        std::shared_ptr<GraphicsDevice> _device;

        // Temporary list to collect lights to render shadows for
        std::vector<Light*> _shadowLights;
    };
}
