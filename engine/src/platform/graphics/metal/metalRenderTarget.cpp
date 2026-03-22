// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.10.2025.
//
#include "metalRenderTarget.h"

#include "metalGraphicsDevice.h"
#include "metalTexture.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    namespace
    {
        MTL::Texture* createMultisampledTexture(MetalGraphicsDevice* device, const MTL::PixelFormat format,
            const int width, const int height, const int samples)
        {
            auto* descriptor = MTL::TextureDescriptor::alloc()->init();
            descriptor->setTextureType(MTL::TextureType2DMultisample);
            descriptor->setWidth(width);
            descriptor->setHeight(height);
            descriptor->setPixelFormat(format);
            descriptor->setSampleCount(samples);
            descriptor->setStorageMode(MTL::StorageModePrivate);
            descriptor->setUsage(MTL::TextureUsageRenderTarget);
            auto* texture = device->raw()->newTexture(descriptor);
            descriptor->release();
            return texture;
        }

        MTL::Texture* createInternalDepthTexture(MetalGraphicsDevice* device, const MTL::PixelFormat format,
            const int width, const int height)
        {
            auto* descriptor = MTL::TextureDescriptor::alloc()->init();
            descriptor->setTextureType(MTL::TextureType2D);
            descriptor->setWidth(width);
            descriptor->setHeight(height);
            descriptor->setPixelFormat(format);
            descriptor->setStorageMode(MTL::StorageModePrivate);
            descriptor->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
            auto* texture = device->raw()->newTexture(descriptor);
            descriptor->release();
            return texture;
        }
    }

    void ColorAttachment::destroy()
    {
        if (multisampledBuffer) {
            multisampledBuffer->release();
            multisampledBuffer = nullptr;
        }
        texture = nullptr;
    }

    DepthAttachment::DepthAttachment(const MTL::PixelFormat format, const bool hasStencilValue)
        : pixelFormat(format), hasStencil(hasStencilValue)
    {
    }

    void DepthAttachment::destroy(MetalGraphicsDevice* device)
    {
        (void)device;
        if (depthTextureInternal && depthTexture) {
            depthTexture->release();
        }
        depthTextureInternal = false;
        depthTexture = nullptr;

        if (multisampledDepthBuffer) {
            multisampledDepthBuffer->release();
            multisampledDepthBuffer = nullptr;
        }
    }

    MetalRenderTarget::MetalRenderTarget(const RenderTargetOptions& options)
        : RenderTarget(options)
    {
        createFrameBuffers();
    }

    MetalRenderTarget::~MetalRenderTarget()
    {
        destroyFrameBuffers();
    }

    void MetalRenderTarget::ensureAttachments()
    {
        if (_colorAttachments.size() != static_cast<size_t>(colorBufferCount())) {
            createFrameBuffers();
            return;
        }

        // Refresh cached MTL::Texture* pointers in case the underlying Texture was
        // resized (which recreates the MTL::Texture GPU object, invalidating our
        // cached raw pointer).  This is cheaper than calling createFrameBuffers()
        // because it only updates the pointer, not reallocating attachments.
        for (int i = 0; i < colorBufferCount(); ++i) {
            auto* colorBuffer = getColorBuffer(i);
            if (!colorBuffer || i >= static_cast<int>(_colorAttachments.size()) || !_colorAttachments[i]) continue;
            auto* hwTexture = dynamic_cast<gpu::MetalTexture*>(colorBuffer->impl());
            if (hwTexture && hwTexture->raw() && _colorAttachments[i]->texture != hwTexture->raw()) {
                _colorAttachments[i]->texture = hwTexture->raw();
                _colorAttachments[i]->pixelFormat = hwTexture->raw()->pixelFormat();
            }
        }

        if (hasDepth() && !_depthAttachment) {
            createFrameBuffers();
        }
    }

    void MetalRenderTarget::destroyFrameBuffers()
    {
        for (const auto& colorAttachment : _colorAttachments) {
            if (colorAttachment) {
                colorAttachment->destroy();
            }
        }
        _colorAttachments.clear();

        if (_depthAttachment) {
            auto* metalDevice = dynamic_cast<MetalGraphicsDevice*>(device());
            _depthAttachment->destroy(metalDevice);
            _depthAttachment.reset();
        }
    }

    void MetalRenderTarget::createFrameBuffers()
    {
        auto* metalDevice = dynamic_cast<MetalGraphicsDevice*>(device());
        if (!metalDevice) {
            spdlog::error("MetalRenderTarget requires MetalGraphicsDevice");
            return;
        }

        destroyFrameBuffers();

        _colorAttachments.resize(colorBufferCount());

        for (int i = 0; i < colorBufferCount(); ++i) {
            auto* colorBuffer = getColorBuffer(i);
            if (!colorBuffer) {
                continue;
            }

            colorBuffer->upload();
            auto* hwTexture = dynamic_cast<gpu::MetalTexture*>(colorBuffer->impl());
            if (!hwTexture || !hwTexture->raw()) {
                spdlog::warn("MetalRenderTarget color buffer {} has no Metal texture", i);
                continue;
            }

            auto colorAttachment = std::make_shared<ColorAttachment>();
            colorAttachment->texture = hwTexture->raw();
            colorAttachment->pixelFormat = colorAttachment->texture->pixelFormat();

            if (samples() > 1) {
                colorAttachment->multisampledBuffer = createMultisampledTexture(
                    metalDevice, colorAttachment->pixelFormat, width(), height(), samples()
                );
            }

            _colorAttachments[i] = colorAttachment;
        }

        if (auto* depthTexture = depthBuffer()) {
            depthTexture->upload();
            auto* hwTexture = dynamic_cast<gpu::MetalTexture*>(depthTexture->impl());
            if (hwTexture && hwTexture->raw()) {
                const auto depthFormat = hwTexture->raw()->pixelFormat();
                const bool hasStencil = depthFormat == MTL::PixelFormatDepth32Float_Stencil8;
                _depthAttachment = std::make_shared<DepthAttachment>(depthFormat, hasStencil);
                _depthAttachment->depthTexture = hwTexture->raw();

                if (samples() > 1) {
                    _depthAttachment->multisampledDepthBuffer = createMultisampledTexture(
                        metalDevice, depthFormat, width(), height(), samples()
                    );
                }
            } else {
                spdlog::warn("MetalRenderTarget depth buffer has no Metal texture");
            }
        } else if (hasDepth()) {
            const MTL::PixelFormat depthFormat = hasStencil() ? MTL::PixelFormatDepth32Float_Stencil8 : MTL::PixelFormatDepth32Float;
            _depthAttachment = std::make_shared<DepthAttachment>(depthFormat, hasStencil());
            _depthAttachment->depthTexture = createInternalDepthTexture(metalDevice, depthFormat, width(), height());
            _depthAttachment->depthTextureInternal = true;

            if (samples() > 1) {
                _depthAttachment->multisampledDepthBuffer = createMultisampledTexture(
                    metalDevice, depthFormat, width(), height(), samples()
                );
            }
        }
    }
}
