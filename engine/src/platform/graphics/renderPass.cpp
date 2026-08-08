// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.09.2025.
//
#include <cmath>

#include "graphicsDevice.h"
#include "renderPass.h"

namespace visutwin::canvas
{
    void RenderPass::frameUpdate() const
    {
        if (_options != nullptr && _renderTarget != nullptr) {
            // With no explicit resizeSource the pass follows the back buffer. Only the
            // Metal backend publishes a back-buffer RenderTarget, so on Vulkan this is
            // null and dereferencing it segfaulted the whole camera-frame subtree —
            // taking post-processing, DOF and bloom down with it on the first frame.
            // Falling through with a null source skips the resize, which is the
            // documented contract: RenderPassCameraFrame::frameUpdate() resizes its own
            // scene targets from the device size precisely for this case.
            const auto& backBuffer = _device->backBuffer();
            const Texture* resizeSource = _options->resizeSource != nullptr
                ? _options->resizeSource.get()
                : (backBuffer != nullptr ? backBuffer->getColorBuffer(0) : nullptr);
            if (resizeSource) {
                const auto width = std::floor(resizeSource->width() * scaleX());
                const auto height = std::floor(resizeSource->height() * scaleY());
                _renderTarget->resize(width, height);
            }
        }
    }

    void RenderPass::render() {
        if (_enabled) {
            // null means backbuffer (real pass), "uninitialized" means no pass.
            const bool realPass = _renderTargetInitialized;

            // Nothing this frame can be recorded, so the pass cannot achieve
            // anything. Skip it whole rather than running before()/after()
            // around an execute() that cannot happen: that keeps paired
            // notifications (a subclass firing "prerender" from before() and
            // "postrender" from after()) balanced — both are simply absent for
            // a frame that draws nothing — instead of emitting a prerender
            // whose postrender reports on rendering that never occurred.
            if (realPass && !_device->frameRenderable()) {
                return;
            }

            log(_device, _device->_renderPassIndex);

            _executeSkipped = !_executeEnabled;

            before();

            if (_executeEnabled) {
                bool passReady = true;

                if (realPass && !_skipStart) {
                    _device->startRenderPass(this);
                    passReady = _device->insideRenderPass();
                }

                if (passReady) {
                    execute();
                } else {
                    // The device declined to begin the pass — e.g. a Vulkan
                    // frame skipped at swapchain acquire. after() still runs,
                    // so record that execute() was skipped on purpose.
                    _executeSkipped = true;
                }

                if (realPass && !_skipEnd && passReady) {
                    _device->endRenderPass(this);
                }
            }

            after();

            _device->_renderPassIndex++;
        }
    }

    void RenderPass::setEnabled(bool value) {
        if (_enabled != value) {
            _enabled = value;
            if (value) {
                onEnable();
            } else {
                onDisable();
            }
        }
    }

    void RenderPass::init(const std::shared_ptr<RenderTarget>& renderTarget, const std::shared_ptr<RenderPassOptions>& options)
    {
        setOptions(options);

        // null represents the default framebuffer
        _renderTarget = renderTarget;
        _renderTargetInitialized = true;

        // defaults depend on multisampling
        _samples = std::max(renderTarget ? renderTarget->samples() : _device->samples(), 1);

        // allocate ops only when render target is used (when this function was called)
        allocateAttachments();

        // allow for post-init setup
        postInit();
    }

    void RenderPass::setOptions(const std::shared_ptr<RenderPassOptions>& value)
    {
        _options = value;

        // Sanitize options
        if (value) {
            if (_options->scaleX == 0.0f)
            {
                _options->scaleX = 1.0f;
            }
            if (_options->scaleY == 0.0f)
            {
                _options->scaleY = 1.0f;
            }
        }
    }

    void RenderPass::allocateAttachments()
    {
        // depth
        _depthStencilOps = std::make_shared<DepthStencilAttachmentOps>();

        // if a RT is used (so not a backbuffer) that was created with a user supplied depth buffer,
        // assume the user wants to use its content, and so store it by default
        if (_renderTarget && _renderTarget->hasDepthBuffer()) {
            _depthStencilOps->storeDepth = true;
        }

        // color
        int numColorOps = _renderTarget ? _renderTarget->colorBufferCount() : 1;
        _colorArrayOps.clear();
        _colorArrayOps.resize(numColorOps);

        for (int i = 0; i < numColorOps; i++) {
            auto colorOps = std::make_shared<ColorAttachmentOps>();
            _colorArrayOps[i] = colorOps;

            // if rendering to a single-sampled buffer, this buffer needs to be stored
            if (_samples == 1) {
                colorOps->store = true;
                colorOps->resolve = false;
            }

            // if the render target needs mipmaps
            if (_renderTarget && _renderTarget->hasMipmaps() && i < _renderTarget->colorBufferCount()) {
                auto colorBuffer = _renderTarget->getColorBuffer(i);
                if (colorBuffer && colorBuffer->mipmaps()) {
                    bool intFormat = isIntegerPixelFormat(colorBuffer->format());
                    colorOps->genMipmaps = !intFormat;  // no automatic mipmap generation for integer formats
                }
            }
        }
    }

    void RenderPass::setClearColor(const Color* color)
    {
        // In case of MRT, clear all color attachments.
        for (const auto& colorOps : _colorArrayOps) {
            if (!colorOps) {
                continue;
            }

            if (color) {
                colorOps->clearValue = *color;
                colorOps->clearValueLinear = *color;
            }

            colorOps->clear = color != nullptr;
        }
    }

    void RenderPass::setClearDepth(const float* depthValue)
    {
        if (!_depthStencilOps) {
            return;
        }

        if (depthValue) {
            _depthStencilOps->clearDepthValue = *depthValue;
        }
        _depthStencilOps->clearDepth = depthValue != nullptr;
    }

    void RenderPass::setClearStencil(const int* stencilValue)
    {
        if (!_depthStencilOps) {
            return;
        }

        if (stencilValue) {
            _depthStencilOps->clearStencilValue = *stencilValue;
        }
        _depthStencilOps->clearStencil = stencilValue != nullptr;
    }

    void RenderPass::log(std::shared_ptr<GraphicsDevice> device, int index) const
    {
        const auto& rt = _renderTarget == nullptr ? nullptr : device->backBuffer();
        bool isBackBuffer = false; // Simplified check
        int numColor = 0;
        bool hasDepth = false;
        bool hasStencil = false;
        int mipLevel = 0;

        if (rt) {
            numColor = rt->colorBufferCount();
            hasDepth = rt->hasDepthBuffer();
            hasStencil = rt->hasStencil();
            mipLevel = rt->mipLevel();
        }

        // This is a simplified version of the debug output
        // In a real implementation, you'd use a proper logging system
        std::string rtInfo;
        if (rt) {
            rtInfo = " RT: " + rt->name();
            if (numColor > 0) {
                rtInfo += " [Color";
                if (numColor > 1) {
                    rtInfo += " x " + std::to_string(numColor);
                }
                rtInfo += "]";
            }
            if (hasDepth)
            {
                rtInfo += "[Depth]";
            }
            if (hasStencil)
            {
                rtInfo += "[Stencil]";
            }
            rtInfo += " " + std::to_string(rt->width()) + " x " + std::to_string(rt->height());
            if (_samples > 0) {
                rtInfo += " samples: " + std::to_string(_samples);
            }
            if (mipLevel > 0) {
                rtInfo += " mipLevel: " + std::to_string(mipLevel);
            }
        }

        std::string indexString = _skipStart ? "++" : std::to_string(index);
    }

    std::shared_ptr<ColorAttachmentOps> RenderPass::colorOps() const
    {
        if (!_colorArrayOps.empty()) {
            return _colorArrayOps[0];
        }
        return nullptr;
    }

    void RenderPass::addBeforePass(const std::shared_ptr<RenderPass>& renderPass)
    {
        if (renderPass) {
            _beforePasses.push_back(renderPass);
        }
    }

    void RenderPass::addAfterPass(const std::shared_ptr<RenderPass>& renderPass)
    {
        if (renderPass) {
            _afterPasses.push_back(renderPass);
        }
    }

    void RenderPass::clearBeforePasses()
    {
        _beforePasses.clear();
    }

    void RenderPass::clearAfterPasses()
    {
        _afterPasses.clear();
    }
}
