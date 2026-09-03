// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#include "sceneGrab.h"

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    Texture* ensureGrabTexture(GraphicsDevice* device, RenderTarget* source,
        std::shared_ptr<Texture>& destination, const bool depth, const bool mipmaps,
        const char* name)
    {
        if (!device) {
            return nullptr;
        }

        const Texture* sourceTexture = nullptr;
        if (source) {
            sourceTexture = depth ? source->depthBuffer()
                                  : (source->colorBufferCount() > 0
                                      ? source->getColorBuffer(0) : nullptr);
        }

        // Fall back to the back buffer's geometry: copying from it is legal, but
        // it has no Texture to read a size or format off.
        const RenderTarget* sizeSource = source ? source : device->backBuffer().get();
        const uint32_t width = sourceTexture ? sourceTexture->width()
            : static_cast<uint32_t>(sizeSource ? sizeSource->width() : 0);
        const uint32_t height = sourceTexture ? sourceTexture->height()
            : static_cast<uint32_t>(sizeSource ? sizeSource->height() : 0);
        if (width == 0 || height == 0) {
            return nullptr;
        }

        const PixelFormat format = sourceTexture ? sourceTexture->format()
            : (depth ? device->backBufferDepthFormat() : device->backBufferColorFormat());

        if (!destination || destination->width() != width ||
            destination->height() != height || destination->format() != format) {
            TextureOptions options;
            options.name = name;
            options.width = width;
            options.height = height;
            options.format = format;
            options.mipmaps = mipmaps;
            destination = std::make_shared<Texture>(device, options);
            destination->upload();
        }
        return destination.get();
    }
}
