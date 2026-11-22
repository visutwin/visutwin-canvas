// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// MetalGraphicsDevice: the frame (pacing, drawable, present) and the open command buffer.
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
    void MetalGraphicsDevice::onFrameStart()
    {
        if (!_metalLayer) {
            return;
        }

        // Push a fresh autorelease pool for this frame's metal-cpp
        // autoreleased objects (command buffers, render pass descriptors,
        // drawable queries, etc.). Drained in onFrameEnd — same-frame
        // scope only. Earlier revisions kept the pool alive across frames
        // and drained at the next frameStart, but that is incompatible
        // with delegate-driven hosts (e.g. AppKit MTKView) whose outer
        // autorelease pool pops the nested canvas pool when the delegate
        // callback returns.
        _framePool = NS::AutoreleasePool::alloc()->init();
        _insideFrame = true;
        _frameEverStarted = true;

        // Wait until the frame kMaxInflightFrames back has completed, then advance the
        // rings onto the region it used. waitForNextFrame may already have taken the slot,
        // before the application polled input.
        if (!_frameSlotTaken) {
            waitServicingRunLoop([this] { _frameGate.waitForFrame(); });
        }
        _frameSlotTaken = false;
        deliverReadbacks();
        _transformRing->beginFrame();
        _uniformRing->beginFrame();
        _paletteRing->beginFrame();
        _pendingPaletteOffset = SIZE_MAX;
        _bindlessFrameSlot = (_bindlessFrameSlot + 1) % _bindlessFrameBuffers.size();
        syncBindlessTable();
        // The cluster buffer sets advance with the rings: the slot this frame writes was
        // last used kMaxInflightFrames frames ago, which the frame gate has just
        // waited out.
        _clusterFrameSlot = (_clusterFrameSlot + 1) % _clusterBufferSets.size();
        _clusterSetsUsed = 0;
        _clusterLastLightData = nullptr;
        _clusterLastCellData = nullptr;
        // A pass that starts before this frame binds a grid gets the stand-ins, not the
        // pair last frame bound.
        _clusterBuffersSet = false;
        // After beginFrame, which is where a ring that overflowed last frame grows.
        setBackendBufferVram(_transformRing->totalSize() + _uniformRing->totalSize() + _paletteRing->totalSize(),
            clusterBufferVram());

        // GPU profiler: rotate frame slot + resolve the slot from 2 frames ago.
        if (_metalGpuProfiler) {
            _metalGpuProfiler->beginFrame(renderVersion());
        }

        // Release the previous frame's cached drawable so the first back-buffer
        // render pass of this frame acquires a fresh one.
        _frameDrawable = nullptr;

        const auto [w, h] = size();
        const CGSize drawableSize = _metalLayer->drawableSize();
        if (static_cast<int>(drawableSize.width) != w || static_cast<int>(drawableSize.height) != h) {
            _metalLayer->setDrawableSize(CGSize{static_cast<CGFloat>(w), static_cast<CGFloat>(h)});
        }

        // A drawable waitForNextFrame acquired becomes this frame's, owned by the frame's
        // pool like one acquired here. One acquired before a resize is the old size and is
        // given back; the first back-buffer pass then acquires one at the new size.
        if (_preparedDrawable) {
            const MTL::Texture* texture = _preparedDrawable->texture();
            if (texture && static_cast<int>(texture->width()) == w && static_cast<int>(texture->height()) == h) {
                _frameDrawable = _preparedDrawable;
                _frameDrawable->autorelease();
            } else {
                _preparedDrawable->release();
            }
            _preparedDrawable = nullptr;
        }
    }

    void MetalGraphicsDevice::waitForNextFrame()
    {
        if (!_metalLayer || _insideFrame) {
            return;
        }
        const bool takeSlot = !_frameSlotTaken;
        const bool takeDrawable = !_preparedDrawable;
        if (!takeSlot && !takeDrawable) {
            return;
        }
        // The drawable is acquired at the size the frame will render.
        if (takeDrawable) {
            const auto [w, h] = size();
            const CGSize drawableSize = _metalLayer->drawableSize();
            if (static_cast<int>(drawableSize.width) != w || static_cast<int>(drawableSize.height) != h) {
                _metalLayer->setDrawableSize(CGSize{static_cast<CGFloat>(w), static_cast<CGFloat>(h)});
            }
        }
        // The frame slot first (the GPU catching up, which is also what frees a drawable),
        // then the drawable (the display), both with the run loop kept serviced: a main
        // thread blocked here when the pointer is pressed makes macOS hold the press
        // (runLoopWait.h). nextDrawable may be called from any thread; the pool is the
        // waiting thread's own.
        CA::MetalDrawable* acquired = nullptr;
        waitServicingRunLoop([&] {
            if (takeSlot) {
                _frameGate.waitForFrame();
            }
            if (takeDrawable) {
                NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
                if (CA::MetalDrawable* drawable = _metalLayer->nextDrawable()) {
                    acquired = drawable->retain();
                }
                pool->release();
            }
        });
        _frameSlotTaken = true;
        if (takeDrawable) {
            _preparedDrawable = acquired;
        }
    }

    void MetalGraphicsDevice::captureDrawable(CA::MetalDrawable* drawable)
    {
        MTL::Texture* source = drawable ? drawable->texture() : nullptr;
        if (!source || !_commandQueue) {
            _pendingScreenshotPath.clear();
            return;
        }

        const NS::UInteger width = source->width();
        const NS::UInteger height = source->height();

        // The same readback the public Texture::read seam uses — the drawable is
        // device-private on Apple Silicon, so this has to go through a shared
        // staging texture, and there is no reason for two spellings of that.
        // The drawable is an MTL::Texture with no engine Texture wrapping it,
        // which is why this calls the helper rather than the seam.
        const NS::UInteger rowPitch = width * 4;
        std::vector<uint8_t> pixels(static_cast<size_t>(rowPitch) * height);
        const gpu::TextureReadRegion region{0, 0, static_cast<uint32_t>(width),
            static_cast<uint32_t>(height), 0, 0};
        if (!gpu::readMetalTexture(_device, _commandQueue, source, region, 4,
                pixels.data(), pixels.size())) {
            _pendingScreenshotPath.clear();
            return;
        }

        // The drawable is BGRA8; PNG wants RGBA.
        const bool swap = source->pixelFormat() == MTL::PixelFormatBGRA8Unorm
            || source->pixelFormat() == MTL::PixelFormatBGRA8Unorm_sRGB;
        writeScreenshotPng(_pendingScreenshotPath, static_cast<uint32_t>(width),
            static_cast<uint32_t>(height), static_cast<uint32_t>(rowPitch),
            pixels.data(), swap);
        _pendingScreenshotPath.clear();
    }

    void MetalGraphicsDevice::onFrameEnd()
    {
        // A capture reads the finished drawable back through a buffer of its own, so
        // everything that draws into it has to be committed first. It runs BEFORE the
        // frame's last buffer is committed so the copy is ordered ahead of the present,
        // not after it (a presented drawable may be recycled). Blocking here is
        // acceptable: it happens on the single frame a capture was asked for.
        if (screenshotPending() && _frameDrawable) {
            flushCommands();
            captureDrawable(_frameDrawable);
        }

        // The frame's LAST command buffer: the open one, which holds whatever has not
        // been committed yet, or an empty one when everything already was. It carries
        // the ring completion handlers and the present, and one is committed every
        // frame whatever happened — with no back-buffer pass (post-processing toggled
        // off with stale render actions) _frameDrawable is null, and the frame gate
        // still has to get its slot back or it blocks for good after
        // kMaxInflightFrames.
        auto* endBuffer = openCommandBuffer();
        if (endBuffer) {
            // After everything the frame rendered.
            encodeReadbacks(endBuffer);
            // The gate's slot comes back when the whole frame has completed on the GPU.
            _frameGate.releaseFrameOnCompletion(endBuffer);

            if (_frameDrawable) {
                // Present only after this buffer — and therefore every prior
                // command buffer on the queue — has COMPLETED on the GPU.
                // presentDrawable() fires when the buffer is merely SCHEDULED; a frame
                // can be committed in more than one buffer (an early commit after a
                // heavy pass), and a scheduled-time present can then scan out a
                // partially rendered drawable when the layer takes the
                // direct-to-display path (visible as flashing patches of the
                // earlier passes' content; screen captures never show it since
                // compositing reads the surface after completion).
                CA::MetalDrawable* presentedDrawable = _frameDrawable;
                presentedDrawable->retain();
                endBuffer->addCompletedHandler([presentedDrawable](MTL::CommandBuffer*) {
                    presentedDrawable->present();
                    presentedDrawable->release();
                });
            }
            flushCommands();
        } else {
            // Balance the frame gate's wait even if Metal cannot allocate the sentinel
            // command buffer. Otherwise the fourth such frame deadlocks.
            spdlog::error("Failed to allocate end-of-frame Metal command buffer");
            _frameGate.releaseFrameOnCompletion(nullptr);
        }

        _insideFrame = false;

        // The drawable has either been retained by the completion handler above or
        // was never acquired. Do not expose an autoreleased pointer after the frame
        // pool is drained below.
        _frameDrawable = nullptr;

        // Drain this frame's autorelease pool. All command buffers /
        // render pass descriptors / drawable queries created between
        // onFrameStart and onFrameEnd are committed by now, so it is
        // safe to release them as a batch.
        if (_framePool) {
            _framePool->release();
            _framePool = nullptr;
        }
    }


    void MetalGraphicsDevice::beginOfflineWork()
    {
        ++_envBatchDepth;
    }

    void MetalGraphicsDevice::endOfflineWork()
    {
        if (_envBatchDepth > 0 && --_envBatchDepth > 0) {
            return;  // inner scope of a nested batch
        }
        // The work is in the open command buffer. Inside a frame it is committed with
        // the frame, in order; outside one it is committed here. NOT waited on either
        // way: in-engine callers only sample the result from later GPU work on the
        // same queue, which is already ordered, and graphics/reflection-cubemap re-bakes
        // every frame — a stall there would serialise CPU and GPU for nothing.
        // Vulkan's side waits because it must, to reuse frame-scoped resources safely,
        // not because callers need completion.
        if (!_insideFrame) {
            flushCommands();
        }
    }

    MTL::CommandBuffer* MetalGraphicsDevice::openCommandBuffer()
    {
        if (!_commandQueue || _renderPassEncoder) {
            return nullptr;
        }
        if (!_openCommandBuffer) {
            // Autoreleased, and it may outlive the pool it was made in (work encoded
            // before the frame starts stays open into it), so it is retained.
            _openCommandBuffer = _commandQueue->commandBuffer();
            if (_openCommandBuffer) {
                _openCommandBuffer->retain();
            }
            _openBufferDraws = 0;
            _openBufferVertices = 0;
        }
        return _openCommandBuffer;
    }

    bool MetalGraphicsDevice::flushCommands()
    {
        if (_renderPassEncoder) {
            return false;
        }
        if (_openCommandBuffer) {
            _openCommandBuffer->commit();
            _openCommandBuffer->release();
            _openCommandBuffer = nullptr;
        }
        return true;
    }
}
