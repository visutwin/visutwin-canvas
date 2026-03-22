// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#pragma once

#include "renderPassShaderQuad.h"

namespace visutwin::canvas
{
    class RenderPassUpsample : public RenderPassShaderQuad
    {
    public:
        RenderPassUpsample(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture);

        void execute() override;

    private:
        Texture* _sourceTexture = nullptr;
    };
}
