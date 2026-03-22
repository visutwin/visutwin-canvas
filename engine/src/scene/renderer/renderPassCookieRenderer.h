// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 01.10.2025.
//
#pragma once

#include "platform/graphics/renderPass.h"
#include "scene/light.h"

namespace visutwin::canvas
{
    /**
     * A render pass used to render cookie textures (both 2D and Cubemap) into the texture atlas.
     */
    class RenderPassCookieRenderer : public RenderPass
    {
    public:
        explicit RenderPassCookieRenderer(const std::shared_ptr<GraphicsDevice>& device) : RenderPass(device), _forceCopy(false) {}

        void update(const std::vector<Light*>& lights);

    private:
        void filter(const std::vector<Light*>& lights, std::vector<Light*>& filteredLights);

        std::vector<Light*> _filteredLights;

        bool _forceCopy;

        bool _executeEnabled = true;
    };
}
