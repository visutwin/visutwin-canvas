// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
#include "colorTarget.h"

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    std::shared_ptr<Texture> createColorTexture(GraphicsDevice* device, const std::string& name,
        const PixelFormat format, const int width, const int height)
    {
        TextureOptions options;
        options.name = name;
        options.width = width;
        options.height = height;
        options.format = format;
        options.mipmaps = false;
        options.minFilter = FilterMode::FILTER_LINEAR;
        options.magFilter = FilterMode::FILTER_LINEAR;
        auto texture = std::make_shared<Texture>(device, options);
        texture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        texture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        return texture;
    }

    ColorTarget createColorTarget(GraphicsDevice* device, const std::string& name, const PixelFormat format,
        const int width, const int height, const std::string& targetName)
    {
        ColorTarget result;
        result.texture = createColorTexture(device, name, format, width, height);
        RenderTargetOptions options;
        options.graphicsDevice = device;
        options.colorBuffer = result.texture.get();
        options.depth = false;
        options.stencil = false;
        options.name = targetName.empty() ? name : targetName;
        result.target = device->createRenderTarget(options);
        return result;
    }
}
