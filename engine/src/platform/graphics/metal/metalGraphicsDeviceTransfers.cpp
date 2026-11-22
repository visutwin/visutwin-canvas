// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// MetalGraphicsDevice: render-target copies, mip generation, texture copies and readbacks.
#include <cmath>
#include "metalGraphicsDevice.h"
#include "platform/graphics/bindlessTable.h"

#include "platform/graphics/textureSamplerState.h"
#include "platform/graphics/runLoopWait.h"
#include <typeinfo>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ranges>
#include "metalTexture.h"
#include "core/scopedTimer.h"
#include "platform/graphics/screenshot.h"
#include "metalComputePipeline.h"
#include "metalIndexBuffer.h"
#include "metalShader.h"
#include "metalRenderPipeline.h"
#include "metalRenderTarget.h"
#include "metalUtils.h"
#include "metalVertexBuffer.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/stencilParameters.h"
#include "platform/graphics/texture.h"
#include "scene/materials/material.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    void MetalGraphicsDevice::copyRenderTarget(RenderTarget* source,
        Texture* colorDestination, Texture* depthDestination)
    {
        if (!_commandQueue || (!colorDestination && !depthDestination)) {
            return;
        }

        // Resolve each source: the render target's own attachment, or the frame's
        // drawable / back-buffer depth when copying from the back buffer.
        const auto resolveColor = [&]() -> MTL::Texture* {
            if (source && source->colorBufferCount() > 0) {
                if (const auto* colorBuffer = source->getColorBuffer(0)) {
                    if (auto* hw = dynamic_cast<gpu::MetalTexture*>(colorBuffer->impl())) {
                        return hw->raw();
                    }
                }
            }
            return _frameDrawable ? _frameDrawable->texture() : nullptr;
        };
        const auto resolveDepth = [&]() -> MTL::Texture* {
            if (source && source->depthBuffer()) {
                if (auto* hw = dynamic_cast<gpu::MetalTexture*>(source->depthBuffer()->impl())) {
                    return hw->raw();
                }
            }
            // A target of its own with no depth texture has nothing to copy: the back
            // buffer's depth is another image (an offscreen camera got the window's).
            if (source && source != backBuffer().get()) {
                return nullptr;
            }
            // Under MSAA the attachment is multisampled, which no blit can read; the
            // passes resolve it here whenever they store it.
            return _backBufferDepthResolve ? _backBufferDepthResolve : _backBufferDepthTexture;
        };
        const auto rawOf = [](Texture* texture) -> MTL::Texture* {
            if (!texture) return nullptr;
            texture->upload();
            auto* hw = dynamic_cast<gpu::MetalTexture*>(texture->impl());
            return hw ? hw->raw() : nullptr;
        };

        MTL::Texture* colorSource = colorDestination ? resolveColor() : nullptr;
        MTL::Texture* depthSource = depthDestination ? resolveDepth() : nullptr;
        MTL::Texture* colorTarget = rawOf(colorDestination);
        MTL::Texture* depthTarget = rawOf(depthDestination);

        const bool doColor = colorSource && colorTarget;
        const bool doDepth = depthSource && depthTarget;
        if (!doColor && !doDepth) {
            return;
        }

        // In the open command buffer, after the pass that produced the source and
        // before the one that samples the copy, because that is the order they are
        // encoded in.
        auto* commandBuffer = openCommandBuffer();
        auto* blitEncoder = commandBuffer ? commandBuffer->blitCommandEncoder() : nullptr;
        if (!blitEncoder) {
            return;
        }
        const auto blit = [blitEncoder](MTL::Texture* from, MTL::Texture* to) {
            blitEncoder->copyFromTexture(from, 0, 0, MTL::Origin(0, 0, 0),
                MTL::Size(from->width(), from->height(), 1),
                to, 0, 0, MTL::Origin(0, 0, 0));
        };
        if (doColor) blit(colorSource, colorTarget);
        if (doDepth) blit(depthSource, depthTarget);
        blitEncoder->endEncoding();
        if (!_insideFrame) {
            flushCommands();
        }
    }

    void MetalGraphicsDevice::generateMipmaps(Texture* texture)
    {
        if (!texture || !_commandQueue) {
            return;
        }
        auto* hw = dynamic_cast<gpu::MetalTexture*>(texture->impl());
        MTL::Texture* raw = hw ? hw->raw() : nullptr;
        if (!raw || raw->mipmapLevelCount() <= 1) {
            return;
        }
        auto* commandBuffer = openCommandBuffer();
        auto* blitEncoder = commandBuffer ? commandBuffer->blitCommandEncoder() : nullptr;
        if (!blitEncoder) {
            return;
        }
        blitEncoder->generateMipmaps(raw);
        blitEncoder->endEncoding();
        // Inside a frame or an offline scope it is submitted with that; alone, here.
        if (!_insideFrame && _envBatchDepth <= 0) {
            flushCommands();
        }
    }

    void MetalGraphicsDevice::queueReadback(MTL::Texture* texture, const gpu::TextureReadRegion& region,
        const uint32_t bytesPerPixel, gpu::TextureReadCallback callback)
    {
        Readback readback;
        readback.texture = texture->retain();
        readback.region = region;
        readback.bytesPerPixel = bytesPerPixel;
        readback.callback = std::move(callback);
        _readbacksQueued.push_back(std::move(readback));
    }

    void MetalGraphicsDevice::encodeReadbacks(MTL::CommandBuffer* commandBuffer)
    {
        if (_readbacksQueued.empty()) {
            return;
        }
        MTL::BlitCommandEncoder* blit = commandBuffer->blitCommandEncoder();
        if (!blit) {
            spdlog::error("MetalGraphicsDevice: no blit encoder for {} texture readbacks", _readbacksQueued.size());
            dropReadbacks();
            return;
        }
        auto done = std::make_shared<std::atomic<bool>>(false);
        for (auto& readback : _readbacksQueued) {
            const auto& region = readback.region;
            const size_t rowPitch = static_cast<size_t>(region.width) * readback.bytesPerPixel;
            readback.buffer = _device->newBuffer(rowPitch * region.height * region.depth,
                MTL::ResourceStorageModeShared);
            if (readback.buffer) {
                gpu::encodeTextureToBuffer(blit, readback.texture, region, rowPitch, readback.buffer);
            }
            readback.texture->release();
            readback.texture = nullptr;
            readback.done = done;
            if (readback.buffer) {
                _readbacksInFlight.push_back(std::move(readback));
            }
        }
        blit->endEncoding();
        _readbacksQueued.clear();
        commandBuffer->addCompletedHandler([done](MTL::CommandBuffer*) { done->store(true); });
    }

    void MetalGraphicsDevice::deliverReadbacks()
    {
        // In order, and only the completed ones; a callback may queue the next read.
        std::vector<Readback> completed;
        for (auto it = _readbacksInFlight.begin(); it != _readbacksInFlight.end();) {
            if (it->done && it->done->load()) {
                completed.push_back(std::move(*it));
                it = _readbacksInFlight.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& readback : completed) {
            const size_t size = static_cast<size_t>(readback.region.width) * readback.region.height *
                readback.region.depth * readback.bytesPerPixel;
            if (readback.callback) {
                readback.callback(static_cast<const uint8_t*>(readback.buffer->contents()), size);
            }
            readback.buffer->release();
        }
    }

    void MetalGraphicsDevice::dropReadbacks()
    {
        for (auto& readback : _readbacksQueued) {
            if (readback.texture) {
                readback.texture->release();
            }
        }
        _readbacksQueued.clear();
        for (auto& readback : _readbacksInFlight) {
            if (readback.buffer) {
                readback.buffer->release();
            }
        }
        _readbacksInFlight.clear();
    }

    bool MetalGraphicsDevice::encodeTextureCopy(MTL::Texture* source, MTL::Texture* destination,
        const gpu::TextureCopyRegion& region)
    {
        if (!source || !destination) {
            return false;
        }
        auto* commandBuffer = openCommandBuffer();
        auto* blitEncoder = commandBuffer ? commandBuffer->blitCommandEncoder() : nullptr;
        if (!blitEncoder) {
            spdlog::warn("MetalGraphicsDevice: a texture copy cannot be encoded inside a render pass");
            return false;
        }
        // A volume's slices are its z range, at slice 0.
        const bool volume = source->textureType() == MTL::TextureType3D;
        const NS::UInteger slice = volume ? 0 : region.face;
        const NS::UInteger z = volume ? region.face : 0;
        blitEncoder->copyFromTexture(source, slice, region.sourceMipLevel,
            MTL::Origin(region.sourceX, region.sourceY, z),
            MTL::Size(region.width, region.height, volume ? region.depth : 1),
            destination, slice, region.destMipLevel, MTL::Origin(region.destX, region.destY, z));
        blitEncoder->endEncoding();
        // Inside a frame or an offline scope it is submitted with that; alone, here.
        if (!_insideFrame && _envBatchDepth <= 0) {
            flushCommands();
        }
        return true;
    }
}
