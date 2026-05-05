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

        float blurRadiusNear() const { return _blurRadiusNear; }
        void setBlurRadiusNear(const float value) { _blurRadiusNear = value; }

        float blurRadiusFar() const { return _blurRadiusFar; }
        void setBlurRadiusFar(const float value) { _blurRadiusFar = value; }

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
        float _blurRadiusNear = 1.0f;
        float _blurRadiusFar = 1.0f;
        int _blurRings = 3;
        int _blurRingPoints = 3;
        std::vector<float> _kernel;
    };
}
