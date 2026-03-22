// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#pragma once

#include "../camera.h"
#include "../light.h"

namespace visutwin::canvas
{
    class LightTextureAtlas;
    class Renderer;

    class ShadowRenderer
    {
    public:
        ShadowRenderer(Renderer* renderer, LightTextureAtlas* lightTextureAtlas) : _renderer(renderer), _lightTextureAtlas(lightTextureAtlas) {}

        bool needsShadowRendering(Light* light);

        Camera* prepareFace(Light* light, Camera* camera, int face);

        LightRenderData* getLightRenderData(Light* light, Camera* camera, int face);

        void setupRenderPass(RenderPass* renderPass, Camera* shadowCamera, bool clearRenderTarget);

        // Creates a shadow camera for a light and sets up its constant properties
        static Camera* createShadowCamera(ShadowType shadowType, LightType type, int face);

    private:
        Renderer* _renderer;
        LightTextureAtlas* _lightTextureAtlas;
    };
}