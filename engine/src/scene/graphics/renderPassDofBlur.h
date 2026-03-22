// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#pragma once

#include <vector>

#include "renderPassShaderQuad.h"

namespace visutwin::canvas
{
    class RenderPassDofBlur : public RenderPassShaderQuad
    {
    public:
        RenderPassDofBlur(const std::shared_ptr<GraphicsDevice>& device, Texture* nearTexture, Texture* farTexture, Texture* cocTexture);

        float blurRadiusNear = 1.0f;
        float blurRadiusFar = 1.0f;

        void setBlurRings(int value);
        int blurRings() const { return _blurRings; }

        void setBlurRingPoints(int value);
        int blurRingPoints() const { return _blurRingPoints; }

        void execute() override;

    private:
        void rebuildKernel();

        Texture* _nearTexture = nullptr;
        Texture* _farTexture = nullptr;
        Texture* _cocTexture = nullptr;
        int _blurRings = 3;
        int _blurRingPoints = 3;
        std::vector<float> _kernel;
    };
}

