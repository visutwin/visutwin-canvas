// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis on 01.10.2025.
//
#include "renderPassShadowLocalClustered.h"

namespace visutwin::canvas
{
    void RenderPassShadowLocalClustered::update(const std::vector<Light*>& localLights) {
        // prepare render targets / shadow cameras for rendering
        auto& shadowLights = _shadowRendererLocal->shadowLights();
        auto shadowCamera = _shadowRendererLocal->prepareLights(shadowLights, localLights);

        // if any shadows need to be rendered
        const int count = static_cast<int>(shadowLights.size());
        setEnabled(count > 0);

        if (count > 0) {
            // setup render pass using any of the cameras, they all have the same pass-related properties
            // Note that the render pass is set up to not clear the render target, as individual shadow maps clear it
            _shadowRenderer->setupRenderPass(this, shadowCamera, false);
        }
    }
}