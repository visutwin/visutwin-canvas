// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.10.2025.
//
#pragma once

#include "Metal/Metal.hpp"
#include "platform/graphics/renderTarget.h"

namespace visutwin::canvas
{
    class MetalGraphicsDevice;

    /**
     * Private class storing info about color buffer
     */
    class ColorAttachment {
    public:
        MTL::PixelFormat pixelFormat = MTL::PixelFormatInvalid;

        MTL::Texture* texture = nullptr;

        // Multi-sampled buffer
        MTL::Texture* multisampledBuffer = nullptr;

        ColorAttachment() = default;
        ~ColorAttachment() = default;

        void destroy();
    };

    /**
     * Private class storing info about depth-stencil buffer.
     */
    class DepthAttachment {
    public:
        MTL::PixelFormat pixelFormat = MTL::PixelFormatInvalid;

        // True if the format has stencil
        bool hasStencil = false;

        // Depth texture
        MTL::Texture* depthTexture = nullptr;

        // True if the depthTexture is internally allocated / owned
        bool depthTextureInternal = false;

        // Multi-sampled depth buffer allocated over the user-provided depth buffer
        MTL::Texture* multisampledDepthBuffer = nullptr;

        explicit DepthAttachment(MTL::PixelFormat format, bool hasStencil);

        ~DepthAttachment() = default;

        // Destroy resources
        void destroy(MetalGraphicsDevice* device);
    };

    /**
     * Metal render target implementation.
     * Manages color and depth/stencil textures for off-screen rendering.
     */
    class MetalRenderTarget : public RenderTarget
    {
    public:
        explicit MetalRenderTarget(const RenderTargetOptions& options = {});

        virtual ~MetalRenderTarget();

        const std::vector<std::shared_ptr<ColorAttachment>>& colorAttachments() const { return _colorAttachments; }
        const std::shared_ptr<DepthAttachment>& depthAttachment() const { return _depthAttachment; }
        void ensureAttachments();

    private:
        void destroyFrameBuffers() override;
        void createFrameBuffers() override;

        std::vector<std::shared_ptr<ColorAttachment>> _colorAttachments;
        std::shared_ptr<DepthAttachment> _depthAttachment;
    };
}
