// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#pragma once

#include "renderPassShaderQuad.h"

namespace visutwin::canvas
{
    class RenderPassCompose : public RenderPassShaderQuad
    {
    public:
        explicit RenderPassCompose(const std::shared_ptr<GraphicsDevice>& device)
            : RenderPassShaderQuad(device) {}

        Texture* sceneTexture = nullptr;
        Texture* bloomTexture = nullptr;
        Texture* cocTexture = nullptr;
        Texture* blurTexture = nullptr;
        Texture* ssaoTexture = nullptr;
        bool taaEnabled = false;
        bool blurTextureUpscale = false;
        float bloomIntensity = 0.01f;
        bool dofEnabled = false;
        float dofIntensity = 1.0f;
        float sharpness = 0.0f;
        int toneMapping = 0;
        float exposure = 1.0f;

        void execute() override;
    };
}
