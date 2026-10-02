// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The intermediate every post-processing pass renders into: a colour texture sampled
// linearly, clamped at the edges and without mips, and a colour-only render target over
// it. A RenderTarget only POINTS at its colour buffer, so the texture comes back beside
// the target and the caller keeps both alive.
//
#pragma once

#include <memory>
#include <string>

#include "platform/graphics/constants.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class RenderTarget;
    class Texture;

    struct ColorTarget
    {
        std::shared_ptr<Texture> texture;
        std::shared_ptr<RenderTarget> target;
    };

    /// The texture alone: linear filtering, clamp to edge, one level.
    std::shared_ptr<Texture> createColorTexture(GraphicsDevice* device, const std::string& name,
        PixelFormat format, int width = 1, int height = 1);

    /// The texture and a colour-only target (no depth, no stencil, one sample) over it,
    /// named `targetName`, or the texture's name when that is empty.
    ColorTarget createColorTarget(GraphicsDevice* device, const std::string& name, PixelFormat format,
        int width = 1, int height = 1, const std::string& targetName = {});
}
