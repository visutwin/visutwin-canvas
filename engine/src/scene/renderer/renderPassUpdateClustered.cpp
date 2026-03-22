// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis  on 12.09.2025.
//

#include "renderPassUpdateClustered.h"

namespace visutwin::canvas
{
    RenderPassUpdateClustered::RenderPassUpdateClustered(const std::shared_ptr<GraphicsDevice>& device, Renderer* renderer,
        ShadowRenderer* shadowRenderer, ShadowRendererLocal* shadowRendererLocal, LightTextureAtlas* lightTextureAtlas) :
        RenderPass(device)
    {
        (void)renderer;
        (void)lightTextureAtlas;
        _cookiesRenderPass = std::make_shared<RenderPassCookieRenderer>(device);
        _shadowRenderPass = std::make_shared<RenderPassShadowLocalClustered>(device, shadowRenderer, shadowRendererLocal);
    }

    void RenderPassUpdateClustered::update(FrameGraph* frameGraph, bool shadowsEnabled, bool cookiesEnabled,
                                           const std::vector<Light*>& lights, const std::vector<Light*>& localLights) {
        _frameGraph = frameGraph;

        if (_cookiesRenderPass) {
            _cookiesRenderPass->setEnabled(cookiesEnabled);
        }
        if (cookiesEnabled && _cookiesRenderPass) {
            _cookiesRenderPass->update(lights);
            if (_frameGraph) {
                _frameGraph->addRenderPass(_cookiesRenderPass);
            }
        }

        if (_shadowRenderPass) {
            _shadowRenderPass->setEnabled(shadowsEnabled);
        }
        if (shadowsEnabled && _shadowRenderPass) {
            _shadowRenderPass->update(localLights);
            if (_frameGraph) {
                _frameGraph->addRenderPass(_shadowRenderPass);
            }
        }
    }
}
