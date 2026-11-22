// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// MetalGraphicsDevice: render pass setup and teardown, viewport and scissor.
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
    namespace
    {
        MTL::StoreAction storeAction(const bool store, const bool resolve)
        {
            if (resolve) {
                return store ? MTL::StoreActionStoreAndMultisampleResolve : MTL::StoreActionMultisampleResolve;
            }
            return store ? MTL::StoreActionStore : MTL::StoreActionDontCare;
        }

        void applyColorLoad(MTL::RenderPassColorAttachmentDescriptor* attachment, const ColorAttachmentOps* ops)
        {
            if (ops && ops->clear) {
                const auto c = ops->clearValue;
                attachment->setLoadAction(MTL::LoadActionClear);
                attachment->setClearColor(MTL::ClearColor::Make(c.r, c.g, c.b, c.a));
            } else {
                attachment->setLoadAction(MTL::LoadActionLoad);
            }
        }

        void applyDepthLoad(MTL::RenderPassDepthAttachmentDescriptor* attachment, const DepthStencilAttachmentOps* ops)
        {
            if (ops && ops->clearDepth) {
                attachment->setLoadAction(MTL::LoadActionClear);
                attachment->setClearDepth(ops->clearDepthValue);
            } else {
                attachment->setLoadAction(MTL::LoadActionLoad);
            }
        }

        /// A memoryless multisampled twin has nowhere to be stored to. The pass that
        /// asked was built to load this target later; that load will read nothing,
        /// which is a scene bug rather than a crash — say so, once per kind.
        void warnDroppedTransientStore(const RenderTarget& target, const char* kind, bool& warned)
        {
            if (!warned) {
                warned = true;
                spdlog::warn("Render target '{}' asks to STORE a transient multisampled {} "
                    "attachment; storing is dropped. Create it without transientMultisample "
                    "if a later pass loads it.", target.name(), kind);
            }
        }

        /// The back buffer's attachments. With `msaaColor` (an antialiased back buffer) the
        /// pass draws into it and resolves into the drawable, as upstream's WebGPU back
        /// buffer: the multisampled colour is stored only when a later pass loads it (the
        /// frame graph sets `store`), and the depth is resolved into `depthResolve`
        /// whenever it is stored, so a copy of the back buffer's depth (a depth grab)
        /// reads what the pass drew. A pass with no ops resolves and stores nothing else.
        void configureBackBufferAttachments(MTL::RenderPassDescriptor* passDesc, MTL::Texture* drawableTexture,
            MTL::Texture* msaaColor, MTL::Texture* depthTexture, MTL::Texture* depthResolve,
            const ColorAttachmentOps* colorOps, const DepthStencilAttachmentOps* depthOps)
        {
            auto* colorAttachment = passDesc->colorAttachments()->object(0);
            const bool storeColor = colorOps && colorOps->store;
            if (msaaColor) {
                colorAttachment->setTexture(msaaColor);
                const bool resolve = !colorOps || colorOps->resolve;
                if (resolve) {
                    colorAttachment->setResolveTexture(drawableTexture);
                }
                applyColorLoad(colorAttachment, colorOps);
                colorAttachment->setStoreAction(storeAction(storeColor, resolve));
            } else {
                colorAttachment->setTexture(drawableTexture);
                applyColorLoad(colorAttachment, colorOps);
                colorAttachment->setStoreAction(storeColor ? MTL::StoreActionStore : MTL::StoreActionDontCare);
            }

            if (!depthTexture) {
                return;
            }
            auto* depthAttachment = passDesc->depthAttachment();
            depthAttachment->setTexture(depthTexture);
            applyDepthLoad(depthAttachment, depthOps);
            const bool storeDepth = depthOps && depthOps->storeDepth;
            const bool resolveDepth = storeDepth && depthResolve;
            if (resolveDepth) {
                depthAttachment->setResolveTexture(depthResolve);
                depthAttachment->setDepthResolveFilter(MTL::MultisampleDepthResolveFilterSample0);
            }
            depthAttachment->setStoreAction(storeAction(storeDepth, resolveDepth));

            // The stencil shares the texture and follows the depth: cleared when
            // either is (a pass that starts a fresh depth starts a fresh stencil),
            // kept when either is kept, so UI masks drawn in a later pass never read
            // a stencil the previous pass discarded.
            auto* stencilAttachment = passDesc->stencilAttachment();
            stencilAttachment->setTexture(depthTexture);
            if (depthOps && (depthOps->clearStencil || depthOps->clearDepth)) {
                stencilAttachment->setLoadAction(MTL::LoadActionClear);
                stencilAttachment->setClearStencil(static_cast<uint32_t>(depthOps->clearStencilValue));
            } else {
                stencilAttachment->setLoadAction(MTL::LoadActionLoad);
            }
            stencilAttachment->setStoreAction(depthOps && (depthOps->storeStencil || depthOps->storeDepth)
                ? MTL::StoreActionStore
                : MTL::StoreActionDontCare);
        }

        void configureOffscreenColorAttachment(MTL::RenderPassColorAttachmentDescriptor* colorAttachment,
            const ColorAttachment& attachment, const RenderTarget& target, const ColorAttachmentOps* ops,
            const bool canResolve)
        {
            const bool multisampled = attachment.multisampledBuffer != nullptr;
            colorAttachment->setTexture(multisampled ? attachment.multisampledBuffer : attachment.texture);

            // A multisample attachment is a single-level texture sized for the
            // target mip; otherwise render directly into the requested mip.
            if (!multisampled) {
                colorAttachment->setLevel(static_cast<NS::UInteger>(target.mipLevel()));
            }

            // Cubemaps and 2D arrays select their destination through slice.
            // Over a multisampled texture with no resolve buffer there is no single-sampled
            // texture at all.
            const auto colorTextureType = attachment.texture ? attachment.texture->textureType()
                                                             : MTL::TextureType2DMultisample;
            const bool colorHasSlices = colorTextureType == MTL::TextureTypeCube ||
                colorTextureType == MTL::TextureType2DArray;
            if (!multisampled && colorHasSlices && target.face() >= 0) {
                colorAttachment->setSlice(static_cast<NS::UInteger>(target.face()));
            }
            // A volume's depth slice is its depth plane (a volume target is never multisampled).
            if (colorTextureType == MTL::TextureType3D && target.face() >= 0) {
                colorAttachment->setDepthPlane(static_cast<NS::UInteger>(target.face()));
            }

            applyColorLoad(colorAttachment, ops);

            const bool resolve = multisampled && attachment.texture && canResolve && ops && ops->resolve;
            if (resolve) {
                colorAttachment->setResolveTexture(attachment.texture);
                colorAttachment->setResolveLevel(static_cast<NS::UInteger>(target.mipLevel()));
                if (colorHasSlices && target.face() >= 0) {
                    colorAttachment->setResolveSlice(static_cast<NS::UInteger>(target.face()));
                }
            }
            bool storeColor = ops ? ops->store : true;
            if (multisampled && storeColor && attachment.multisampledMemoryless) {
                static bool warned = false;
                warnDroppedTransientStore(target, "colour", warned);
                storeColor = false;
            }
            colorAttachment->setStoreAction(storeAction(storeColor, resolve));
        }

        void configureOffscreenDepthAttachment(MTL::RenderPassDescriptor* passDesc, const DepthAttachment& depthData,
            const RenderTarget& target, const DepthStencilAttachmentOps* depthOps, const bool canResolve)
        {
            auto* depthAttachment = passDesc->depthAttachment();
            const bool depthMsaa = depthData.multisampledDepthBuffer != nullptr;
            MTL::Texture* depthTex = depthMsaa ? depthData.multisampledDepthBuffer : depthData.depthTexture;
            depthAttachment->setTexture(depthTex);

            // Cubemap face rendering: when the depth texture is a cubemap, target a
            // specific face via the slice parameter.  This enables rendering to individual
            // faces of a point-light shadow cubemap. The same slice mechanism targets an
            // individual layer of a depth texture2d_array — used by the clustered local
            // shadow atlas (LightTextureAtlas), where each spot light owns one array slice.
            if ((depthTex->textureType() == MTL::TextureTypeCube ||
                 depthTex->textureType() == MTL::TextureType2DArray) && target.face() >= 0) {
                depthAttachment->setSlice(static_cast<NS::UInteger>(target.face()));
            }

            applyDepthLoad(depthAttachment, depthOps);

            const bool resolveDepth = depthMsaa && depthData.depthTexture && canResolve && depthOps &&
                depthOps->resolveDepth;
            if (resolveDepth) {
                depthAttachment->setResolveTexture(depthData.depthTexture);
                // Sample0 is the only filter guaranteed everywhere; Min/Max need
                // MTLDevice::supportsDepthResolveFilter and buy nothing here.
                depthAttachment->setDepthResolveFilter(MTL::MultisampleDepthResolveFilterSample0);
            }
            bool storeDepth = depthOps ? depthOps->storeDepth : true;
            if (depthMsaa && storeDepth && depthData.multisampledMemoryless) {
                static bool warnedDepth = false;
                warnDroppedTransientStore(target, "depth", warnedDepth);
                storeDepth = false;
            }
            depthAttachment->setStoreAction(storeAction(storeDepth, resolveDepth));

            if (!depthData.hasStencil) {
                return;
            }
            auto* stencilAttachment = passDesc->stencilAttachment();
            stencilAttachment->setTexture(depthTex);
            if (depthOps && depthOps->clearStencil) {
                stencilAttachment->setLoadAction(MTL::LoadActionClear);
                stencilAttachment->setClearStencil(depthOps->clearStencilValue);
            } else {
                stencilAttachment->setLoadAction(MTL::LoadActionLoad);
            }
            stencilAttachment->setStoreAction(depthOps && depthOps->storeStencil &&
                !(depthMsaa && depthData.multisampledMemoryless) ? MTL::StoreActionStore : MTL::StoreActionDontCare);
        }

        void configureOffscreenAttachments(MTL::RenderPassDescriptor* passDesc, const MetalRenderTarget& target,
            const RenderPass* renderPass)
        {
            static const std::vector<std::shared_ptr<ColorAttachmentOps>> kNoColorOps;
            const auto& colorOpsArray = renderPass ? renderPass->colorArrayOps() : kNoColorOps;
            const auto depthOps = renderPass ? renderPass->depthStencilOps() : nullptr;
            const bool canResolve = target.samples() > 1 && target.autoResolve();

            const auto& colorAttachments = target.colorAttachments();
            for (size_t i = 0; i < colorAttachments.size(); ++i) {
                const auto& attachment = colorAttachments[i];
                if (!attachment || (!attachment->texture && !attachment->multisampledBuffer)) {
                    continue;
                }
                const auto ops = i < colorOpsArray.size() ? colorOpsArray[i] : nullptr;
                configureOffscreenColorAttachment(passDesc->colorAttachments()->object(static_cast<NS::UInteger>(i)),
                    *attachment, target, ops.get(), canResolve);
            }

            const auto& depthData = target.depthAttachment();
            if (depthData && (depthData->depthTexture || depthData->multisampledDepthBuffer)) {
                configureOffscreenDepthAttachment(passDesc, *depthData, target, depthOps.get(), canResolve);
            }
        }
    }

    void MetalGraphicsDevice::startRenderPass(RenderPass* renderPass)
    {
        _psoFormatKeyValid = false;
        if (!_commandQueue || !_metalLayer || _renderPassEncoder) {
            spdlog::warn("Cannot start render pass: queue/layer invalid or encoder already active");
            return;
        }

        const std::shared_ptr<RenderTarget> activeTarget =
            (renderPass && renderPass->renderTarget()) ? renderPass->renderTarget() : backBuffer();
        setRenderTarget(activeTarget);
        const auto offscreenTarget = std::dynamic_pointer_cast<MetalRenderTarget>(activeTarget);
        const bool isBackBufferPass = activeTarget == backBuffer();
        if (!isBackBufferPass && !offscreenTarget) {
            spdlog::error("Non-backbuffer render target is not a MetalRenderTarget");
            return;
        }
        if (offscreenTarget) {
            offscreenTarget->ensureAttachments();
        }

        _currentDrawable = nullptr;
        if (isBackBufferPass && !acquirePassDrawable()) {
            return;
        }

        // The pass encodes into the open command buffer; it gets no buffer of its own.
        _commandBuffer = openCommandBuffer();
        if (!_commandBuffer) {
            spdlog::error("Failed to create Metal command buffer");
            _currentDrawable = nullptr;
            return;
        }

        auto* passDesc = MTL::RenderPassDescriptor::alloc()->init();

        // GPU profiler: sample timestamps at this pass's stage boundaries.
        if (_metalGpuProfiler) {
            _metalGpuProfiler->attachToRenderPass(passDesc,
                renderPass ? renderPass->name() : std::string("backbuffer"));
        }

        if (isBackBufferPass) {
            MTL::Texture* drawableTexture = _currentDrawable->texture();
            ensureBackBufferTextures(static_cast<int>(drawableTexture->width()),
                static_cast<int>(drawableTexture->height()), drawableTexture->pixelFormat());
            const auto colorOps = renderPass ? renderPass->colorOps() : nullptr;
            const auto depthOps = renderPass ? renderPass->depthStencilOps() : nullptr;
            configureBackBufferAttachments(passDesc, drawableTexture, _backBufferMsaaColor,
                _backBufferDepthTexture, _backBufferDepthResolve, colorOps.get(), depthOps.get());
        } else {
            configureOffscreenAttachments(passDesc, *offscreenTarget, renderPass);
        }

        _passHasDepth = passDesc->depthAttachment()->texture() != nullptr;
        _renderPassEncoder = _commandBuffer->renderCommandEncoder(passDesc);
        passDesc->release();
        if (!_renderPassEncoder) {
            spdlog::error("Failed to create Metal render command encoder");
            _commandBuffer = nullptr;
            _currentDrawable = nullptr;
            _insideRenderPass = false;
            return;
        }
        if (renderPass) {
            labelPassEncoder(*renderPass);
        }
        beginPassEncoderState(activeTarget.get());
        _insideRenderPass = true;
    }

    bool MetalGraphicsDevice::acquirePassDrawable()
    {
        // Reuse the frame's cached drawable so that multiple back-buffer render
        // passes within one frame write to the same drawable texture.  Metal's
        // nextDrawable() returns a *different* drawable each call (unlike WebGL's
        // persistent back buffer), so acquiring one per pass would cause only the
        // last pass's content to be visible.
        if (_frameDrawable) {
            _currentDrawable = _frameDrawable;
            spdlog::trace("Reusing cached CAMetalDrawable for back-buffer pass");
            return true;
        }
        // Under display sync this is where the frame waits for the display, and
        // it is inside Engine::render(); see displayWaitMilliseconds().
        {
            const DisplayWaitScope waitScope(*this);
            // Waited for with the run loop serviced (runLoopWait.h); the drawable is retained
            // across the waiting thread's pool and handed to this frame's pool, as one
            // acquired here would be.
            CA::MetalDrawable* acquired = nullptr;
            waitServicingRunLoop([&] {
                NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
                if (CA::MetalDrawable* drawable = _metalLayer->nextDrawable()) {
                    acquired = drawable->retain();
                }
                pool->release();
            });
            _currentDrawable = acquired;
            if (_currentDrawable) {
                _currentDrawable->autorelease();
            }
        }
        if (!_currentDrawable) {
            spdlog::warn("Failed to acquire CAMetalDrawable");
            return false;
        }
        _frameDrawable = _currentDrawable;
        spdlog::trace("Acquired new CAMetalDrawable for frame");
        return true;
    }

    void MetalGraphicsDevice::ensureBackBufferTextures(const int width, const int height,
        const MTL::PixelFormat colorFormat)
    {
        // The back buffer's attachments follow the drawable's size.
        if (_backBufferDepthTexture && _backBufferDepthWidth == width && _backBufferDepthHeight == height) {
            return;
        }
        for (MTL::Texture** texture : {&_backBufferDepthTexture, &_backBufferMsaaColor, &_backBufferDepthResolve}) {
            if (*texture) {
                (*texture)->release();
                *texture = nullptr;
            }
        }
        const int msaa = samples();
        _backBufferDepthTexture = metal::createBackBufferTexture(_device, metal::kBackBufferDepthFormat,
            width, height, msaa);
        if (msaa > 1) {
            _backBufferMsaaColor = metal::createBackBufferTexture(_device, colorFormat, width, height, msaa);
            _backBufferDepthResolve = metal::createBackBufferTexture(_device, metal::kBackBufferDepthFormat,
                width, height, 1);
        }
        _backBufferDepthWidth = width;
        _backBufferDepthHeight = height;
    }

    void MetalGraphicsDevice::labelPassEncoder(RenderPass& renderPass)
    {
        // The pass name on the encoder is what Xcode's frame capture and
        // Instruments' Metal System Trace show per encoder; without it every
        // pass reads as "Render Command N" and a trace cannot be attributed.
        // A pass that never set its name gets its type name. The debug group
        // is what Instruments' GPU timeline actually displays.
        const std::string label = !renderPass.name().empty()
            ? renderPass.name() : std::string(typeid(renderPass).name());
        NS::String* nsLabel = NS::String::string(label.c_str(), NS::UTF8StringEncoding);
        _renderPassEncoder->setLabel(nsLabel);
        _renderPassEncoder->pushDebugGroup(nsLabel);
        _encoderDebugGroupOpen = true;
    }

    void MetalGraphicsDevice::beginPassEncoderState(const RenderTarget* target)
    {
        // A new encoder holds none of the previous one's state.
        resetEncoderStateCache();
        if (MTL::DepthStencilState* initial = _passHasDepth ? _defaultDepthStencilState : _noTestNoWriteDepthStencilState) {
            _renderPassEncoder->setDepthStencilState(initial);
            _encoderDepthStencilState = initial;
        }
        // glTF uses counter-clockwise front faces by default, and
        // nothing draws with the other winding, so it is encoder state set once.
        _renderPassEncoder->setFrontFacingWinding(MTL::WindingCounterClockwise);
        // The vertex stage's one sampler: the material sampler the displacement map reads
        // through (forward-vertex.slang).
        if (_defaultSampler) {
            _renderPassEncoder->setVertexSamplerState(_defaultSampler, 0);
        }
        const int targetWidth = target ? target->width() : size().first;
        const int targetHeight = target ? target->height() : size().second;
        _passWidth = std::max(targetWidth, 0);
        _passHeight = std::max(targetHeight, 0);
        if (targetWidth > 0 && targetHeight > 0) {
            setViewport(0.0f, 0.0f, static_cast<float>(targetWidth), static_cast<float>(targetHeight));
            setScissor(0, 0, targetWidth, targetHeight);
        }

        // Bind ring buffers once per render pass. Per-draw calls will only
        // update the offset (setVertexBufferOffset), which is much cheaper
        // than rebinding the buffer + validating it each time, until a ring grows
        // mid-pass and the binder binds its new buffer.
        _renderPassEncoder->setVertexBuffer(_transformRing->buffer(), 0, 2);
        _renderPassEncoder->setFragmentBuffer(_uniformRing->buffer(), 0, 3);
        _renderPassEncoder->setVertexBuffer(_uniformRing->buffer(), 0, 3);
        _renderPassEncoder->setFragmentBuffer(_uniformRing->buffer(), 0, 4);
        _renderPassEncoder->setVertexBuffer(_uniformRing->buffer(), 0, 4);
        _renderPassEncoder->setVertexBuffer(_paletteRing->buffer(), 0, 6);
        _uniformBinder.noteRingBuffersBound(_transformRing->buffer(), _uniformRing->buffer());

        // Bind clustered lighting buffers at fragment slots 7 (lights) and 8 (cells).
        bindClusterBuffersOnEncoder();

        // Reset per-pass deduplication state for uniforms and textures.
        _uniformBinder.resetPassState();
        _textureBinder.resetPassState();
    }

    void MetalGraphicsDevice::endRenderPass(RenderPass* renderPass)
    {
        if (_renderPassEncoder) {
            _renderPassEncoder->endEncoding();
            _renderPassEncoder = nullptr;
        }
        _insideRenderPass = false;

        const auto activeTarget = renderTarget();
        const auto offscreenTarget = std::dynamic_pointer_cast<MetalRenderTarget>(activeTarget);
        if (_commandBuffer && offscreenTarget && renderPass) {
            bool needsMipmaps = false;
            const auto& colorOpsArray = renderPass->colorArrayOps();
            for (size_t i = 0; i < colorOpsArray.size(); ++i) {
                const auto ops = colorOpsArray[i];
                const auto* colorBuffer = activeTarget && i < static_cast<size_t>(activeTarget->colorBufferCount())
                    ? activeTarget->getColorBuffer(i) : nullptr;
                if (ops && ops->genMipmaps && colorBuffer && colorBuffer->mipmaps()) {
                    needsMipmaps = true;
                    break;
                }
            }

            if (needsMipmaps) {
                auto* blitEncoder = _commandBuffer->blitCommandEncoder();
                if (blitEncoder) {
                    for (size_t i = 0; i < colorOpsArray.size(); ++i) {
                        const auto ops = colorOpsArray[i];
                        const auto* colorBuffer = activeTarget && i < static_cast<size_t>(activeTarget->colorBufferCount())
                            ? activeTarget->getColorBuffer(i) : nullptr;
                        if (!(ops && ops->genMipmaps && colorBuffer && colorBuffer->mipmaps())) {
                            continue;
                        }
                        // A Gaussian chain is rendered by the generator's pass after this one.
                        if (colorBuffer->gaussianMipmaps()) {
                            continue;
                        }
                        auto* hwTexture = dynamic_cast<gpu::MetalTexture*>(colorBuffer->impl());
                        if (!hwTexture || !hwTexture->raw()) {
                            continue;
                        }
                        // An array texture's pass regenerates the chain of the layer it rendered,
                        // through a view of that slice; a cubemap's last face pass all six.
                        MTL::Texture* raw = hwTexture->raw();
                        if (colorBuffer->isArray() && !colorBuffer->isCubemap() && activeTarget->face() >= 0 &&
                            static_cast<NS::UInteger>(activeTarget->face()) < raw->arrayLength()) {
                            MTL::Texture* slice = raw->newTextureView(raw->pixelFormat(), MTL::TextureType2DArray,
                                NS::Range(0, raw->mipmapLevelCount()),
                                NS::Range(static_cast<NS::UInteger>(activeTarget->face()), 1));
                            if (slice) {
                                blitEncoder->generateMipmaps(slice);
                                slice->release();
                                continue;
                            }
                        }
                        blitEncoder->generateMipmaps(raw);
                    }
                    blitEncoder->endEncoding();
                }
            }
        }

        if (!_commandBuffer) {
            spdlog::warn("Render pass ended without a valid command buffer");
        }
        _commandBuffer = nullptr;
        _currentDrawable = nullptr;

        // The pass stays in the open command buffer, which the frame's end commits
        // (and presents from: the drawable is presented once, from the last buffer).
        // Unless it holds a lot of work: then it is committed now, so the GPU starts on
        // a heavy shadow or scene pass while the rest of the frame is still being
        // encoded instead of idling until the frame ends. A pass of a few draws is not
        // worth a buffer of its own; the commit costs about what fifty draws do.
        constexpr uint32_t kEarlyCommitDraws = 256;
        constexpr uint64_t kEarlyCommitVertices = 300'000;
        if (_openBufferDraws >= kEarlyCommitDraws || _openBufferVertices >= kEarlyCommitVertices) {
            flushCommands();
        }
    }

    void MetalGraphicsDevice::setResolution(int width, int height)
    {
        if (_metalLayer) {
            _metalLayer->setDrawableSize(CGSize{static_cast<CGFloat>(width), static_cast<CGFloat>(height)});
        }
    }

    void MetalGraphicsDevice::setViewport(float x, float y, float w, float h)
    {
        GraphicsDevice::setViewport(x, y, w, h);
        if (_renderPassEncoder && w > 0.0f && h > 0.0f) {
            MTL::Viewport viewport;
            viewport.originX = static_cast<double>(x);
            viewport.originY = static_cast<double>(y);
            viewport.width = static_cast<double>(w);
            viewport.height = static_cast<double>(h);
            viewport.znear = 0.0;
            viewport.zfar = 1.0;
            _renderPassEncoder->setViewport(viewport);
        }
    }

    void MetalGraphicsDevice::setScissor(int x, int y, int w, int h)
    {
        GraphicsDevice::setScissor(x, y, w, h);
        if (_renderPassEncoder && w > 0 && h > 0) {
            // Clamped to the pass's attachments, as Vulkan's applyScissor does:
            // a camera rect reaching past the target
            // makes the scissor do the same, and Metal requires the rect to lie within
            // the attachments. Unclamped, a negative x or y would wrap to an enormous
            // NS::UInteger rather than clip.
            const int64_t left = std::clamp<int64_t>(x, 0, _passWidth);
            const int64_t top = std::clamp<int64_t>(y, 0, _passHeight);
            const int64_t right = std::clamp<int64_t>(static_cast<int64_t>(x) + w, left, _passWidth);
            const int64_t bottom = std::clamp<int64_t>(static_cast<int64_t>(y) + h, top, _passHeight);
            MTL::ScissorRect scissor;
            scissor.x = static_cast<NS::UInteger>(left);
            scissor.y = static_cast<NS::UInteger>(top);
            scissor.width = static_cast<NS::UInteger>(right - left);
            scissor.height = static_cast<NS::UInteger>(bottom - top);
            _renderPassEncoder->setScissorRect(scissor);
        }
    }
}
