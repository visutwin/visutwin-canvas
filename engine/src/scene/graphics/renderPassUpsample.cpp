// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
//
#include "renderPassUpsample.h"

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shader.h"
#include "scene/shader-lib/slangShaders.h"

namespace visutwin::canvas
{
    RenderPassUpsample::RenderPassUpsample(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture)
        : RenderPassShaderQuad(device), _sourceTexture(sourceTexture)
    {
        // The first single-source program: engine/shaders/slang/programs/upsample.slang,
        // compiled for this device's backend by the build (or at run time for an
        // override) and cached on the device under its name.
        setShader(getOrCreateSlangShader(device.get(), "upsample"));
    }

    void RenderPassUpsample::execute()
    {
        setQuadTextureBinding(0, _sourceTexture);
        RenderPassShaderQuad::execute();
    }
}
