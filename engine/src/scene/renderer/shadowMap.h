// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauets on 02.10.2025.
//
#pragma once
#include <memory>

#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Light;

    class ShadowMap
    {
    public:
        const std::vector<std::shared_ptr<RenderTarget>>& renderTargets() const { return _renderTargets; }

        // Returns the depth texture used for shadow sampling in the forward pass.
        Texture* shadowTexture() const { return _shadowTexture.get(); }

        // Creates a shadow map for the given light, allocating depth texture and render target.
        static std::unique_ptr<ShadowMap> create(GraphicsDevice* device, Light* light);

    private:
        // An array of render targets:
        // 1 for directional and spotlight
        // 6 for omni light
        std::vector<std::shared_ptr<RenderTarget>> _renderTargets;

        // The depth texture backing the shadow map, owned by this ShadowMap.
        std::shared_ptr<Texture> _shadowTexture;
    };
}
