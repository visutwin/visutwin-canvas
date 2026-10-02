// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
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
