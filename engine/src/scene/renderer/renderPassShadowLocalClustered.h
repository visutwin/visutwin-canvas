// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis  on 01.10.2025.
//
#pragma once

#include "shadowRenderer.h"
#include "shadowRendererLocal.h"
#include "platform/graphics/renderPass.h"
#include "scene/frameGraph.h"
#include "scene/light.h"

namespace visutwin::canvas
{
    /**
    * A render pass used to render local clustered shadows. This is done inside a single render pass,
    * as all shadows are part of a single render target atlas.
     */
    class RenderPassShadowLocalClustered : public RenderPass
    {
    public:
        RenderPassShadowLocalClustered(const std::shared_ptr<GraphicsDevice>& device,
            ShadowRenderer* shadowRenderer, ShadowRendererLocal* shadowRendererLocal)
            : RenderPass(device), _shadowRenderer(shadowRenderer), _shadowRendererLocal(shadowRendererLocal) {}

        // Update the render pass with the current frame's local lights
        void update(const std::vector<Light*>& localLights);

    private:
        ShadowRenderer* _shadowRenderer;
        ShadowRendererLocal* _shadowRendererLocal;
    };
}
