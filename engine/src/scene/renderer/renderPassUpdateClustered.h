// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#pragma once

#include "renderPassCookieRenderer.h"
#include "renderPassShadowLocalClustered.h"
#include "shadowRenderer.h"
#include "shadowRendererLocal.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderPass.h"
#include "scene/frameGraph.h"
#include "scene/light.h"

namespace visutwin::canvas {
    /*
     * A render pass used to update clustered lighting data - shadows, cookies, world clusters
     */
    class RenderPassUpdateClustered : public RenderPass
    {
    public:
        RenderPassUpdateClustered(const std::shared_ptr<GraphicsDevice>& device, Renderer* renderer, ShadowRenderer* shadowRenderer,
            ShadowRendererLocal* shadowRendererLocal, LightTextureAtlas* lightTextureAtlas);

        // Updates the render pass with current lighting data
        void update(FrameGraph* frameGraph, bool shadowsEnabled, bool cookiesEnabled,
            const std::vector<Light*>& lights, const std::vector<Light*>& localLights);

    private:
        FrameGraph* _frameGraph;

        // Render cookies for all local visible lights
        std::shared_ptr<RenderPassCookieRenderer> _cookiesRenderPass;

        // Local shadows - these are shared by all cameras
        std::shared_ptr<RenderPassShadowLocalClustered> _shadowRenderPass;
    };
}
