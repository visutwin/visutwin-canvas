// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#pragma once

#include <memory>

#include "../camera.h"
#include "../light.h"

namespace visutwin::canvas
{
    class LightTextureAtlas;
    class Renderer;

    class ShadowRenderer
    {
    public:
        ShadowRenderer() = default;

        /// Whether this light's shadow map should be re-rendered this frame. PURE —
        /// call it as often as you like. A SHADOWUPDATE_THISFRAME request is consumed
        /// by Renderer::consumeOneShotShadows once the frame graph is built.
        bool needsShadowRendering(const Light* light) const;

        Camera* prepareFace(Light* light, Camera* camera, int face);

        LightRenderData* getLightRenderData(Light* light, Camera* camera, int face);

        void setupRenderPass(RenderPass* renderPass, Camera* shadowCamera, bool clearRenderTarget);

        // Creates a shadow camera for a light and sets up its constant properties
        static std::unique_ptr<Camera> createShadowCamera(ShadowType shadowType, LightType type, int face);

    private:
    };
}
