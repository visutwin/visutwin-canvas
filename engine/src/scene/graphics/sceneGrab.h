// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Destination management for the scene grabs. The device only offers the
// generic `copyRenderTarget` / `generateMipmaps` operations, so deciding WHAT
// to copy into — allocating the texture, resizing it when the target changes,
// matching its format — lives here, once, instead of twice per backend.
//
#pragma once

#include <memory>

namespace visutwin::canvas
{
    class GraphicsDevice;
    class RenderTarget;
    class Texture;

    /**
     * Return a texture matching `source`'s colour (or depth) attachment,
     * (re)creating `destination` when its size or format no longer match.
     * A null `source` means the back buffer, whose format comes from the device.
     */
    Texture* ensureGrabTexture(GraphicsDevice* device, RenderTarget* source,
        std::shared_ptr<Texture>& destination, bool depth, bool mipmaps,
        const char* name);
}
