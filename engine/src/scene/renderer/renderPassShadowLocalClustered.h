// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis on 01.10.2025.
//
#pragma once

#include <memory>
#include <vector>

#include "shadowRendererLocal.h"
#include "platform/graphics/renderPass.h"
#include "scene/frameGraph.h"
#include "scene/light.h"

namespace visutwin::canvas
{
    /**
     * Renders every clustered local shadow for the frame in ONE render pass, because
     * they all live in one render target — the LightTextureAtlas. The pass loads
     * rather than clears: the atlas holds one-shot shadows that must survive while
     * a neighbour re-renders, so each face clears only its own rect (see
     * clearDepthRect) under its viewport and scissor before drawing into it.
     */
    class RenderPassShadowLocalClustered : public RenderPass
    {
    public:
        RenderPassShadowLocalClustered(const std::shared_ptr<GraphicsDevice>& device,
            ShadowRenderer* shadowRenderer, ShadowRendererLocal* shadowRendererLocal)
            : RenderPass(device), _graphicsDevice(device), _shadowRenderer(shadowRenderer),
              _shadowRendererLocal(shadowRendererLocal) {}

        /// Selects the atlas lights whose shadow renders this frame and sets the
        /// pass up on the atlas target. Disables itself when there are none.
        void update(const std::vector<Light*>& localLights);

        void execute() override;

    private:
        std::shared_ptr<GraphicsDevice> _graphicsDevice;
        ShadowRenderer* _shadowRenderer;
        ShadowRendererLocal* _shadowRendererLocal;
    };
}
