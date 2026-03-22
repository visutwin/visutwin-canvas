// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 06.12.2025.
//
#pragma once

#include <platform/graphics/renderPass.h>

#include "shadowRenderer.h"

namespace visutwin::canvas {
    /**
     * A render pass used to render local non-clustered shadows. It represents rendering to a single
     * face of a shadow map, as each face is a separate render target.
     */
    class RenderPassShadowLocalNonClustered : public RenderPass
    {
    public:
        RenderPassShadowLocalNonClustered(const std::shared_ptr<GraphicsDevice>& device, ShadowRenderer* shadowRenderer,
            Light* light, int face, bool applyVsm);
        void execute() override;

    private:
        ShadowRenderer* _shadowRenderer;
        Light* _light;
        Camera* _shadowCamera;
        std::shared_ptr<GraphicsDevice> _graphicsDevice;
        int _face;
        bool _applyVsm;
    };
}
