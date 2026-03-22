// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.09.2025.
//
#include "renderTarget.h"

#include <spdlog/spdlog.h>

#include "graphicsDevice.h"
#include "textureUtils.h"

namespace visutwin::canvas
{
    static int globalRenderTargetId = 0;

    RenderTarget::RenderTarget(const RenderTargetOptions& options)
    {
        _id = globalRenderTargetId++;

        // Get device from one of the buffers
        _device = options.colorBuffer ? options.colorBuffer->device() :
                  (!options.colorBuffers.empty() ? options.colorBuffers[0]->device() :
                  (options.depthBuffer ? options.depthBuffer->device() :
                  options.graphicsDevice));

        assert(_device != nullptr && "Failed to obtain the device, colorBuffer nor depthBuffer store it.");

        // Samples
        int maxSamples = _device->maxSamples();
        _samples = std::min(options.samples, maxSamples);

        // Use the single colorBuffer in the colorBuffers array
        _colorBuffer = options.colorBuffer;
        if (options.colorBuffer) {
            _colorBuffers = {options.colorBuffer};
        }

        // Process optional arguments
        _depthBuffer = options.depthBuffer;
        _face = options.face;

        if (_depthBuffer) {
            auto format = _depthBuffer->format();
            if (format == PixelFormat::PIXELFORMAT_DEPTH || format == PixelFormat::PIXELFORMAT_DEPTH16) {
                _depth = true;
                _stencil = false;
            } else if (format == PixelFormat::PIXELFORMAT_DEPTHSTENCIL) {
                _depth = true;
                _stencil = true;
            } else if (format == PixelFormat::PIXELFORMAT_R32F && _samples > 1) {
                // on WebGPU, when multisampling is enabled, we use R32F format for the specified buffer,
                // which we can resolve depth to using a shader
                _depth = true;
                _stencil = false;
            } else {
                spdlog::warn("Incorrect depthBuffer format. Must be pc.PIXELFORMAT_DEPTH or pc.PIXELFORMAT_DEPTHSTENCIL");
                _depth = false;
                _stencil = false;
            }
        } else {
            _depth = options.depth;
            _stencil = options.stencil;
        }

        // MRT
        if (!options.colorBuffers.empty()) {
            assert((_colorBuffers.empty() || _colorBuffers.size() == 1) &&
                "When constructing RenderTarget and options.colorBuffers is used, options.colorBuffer must not be used.");

            if (_colorBuffers.empty() || _colorBuffers.size() == 1) {
                _colorBuffers = options.colorBuffers;
                // set the main color buffer to point to 0 index
                _colorBuffer = options.colorBuffers[0];
            }
        }

        _autoResolve = options.autoResolve;

        // Use specified name, otherwise get one from color or depth buffer
        _name = options.name;
        if (_name.empty() && _colorBuffer) {
            _name = _colorBuffer->name();
        }
        if (_name.empty() && _depthBuffer) {
            _name = _depthBuffer->name();
        }
        if (_name.empty()) {
            _name = "Untitled";
        }

        // Render image flipped in Y
        _flipY = options.flipY;

        _mipLevel = options.mipLevel;
        if (_mipLevel > 0 && _depth) {
            spdlog::error("Rendering to a mipLevel is not supported when render target uses a depth buffer. Ignoring mipLevel " +
                std::to_string(_mipLevel) + " for render target " + _name);
            _mipLevel = 0;
        }

        // If we render to a specific mipmap (even 0), do not generate mipmaps
        _mipmaps = (options.mipLevel == 0 && !options.colorBuffer);

        validateMrt();

        spdlog::trace("Alloc: Id " + std::to_string(_id) + " " + _name + ": " +
            std::to_string(width()) + "x" + std::to_string(height()) +
            " [samples: " + std::to_string(_samples) + "]" +
            (!_colorBuffers.empty() ? "[MRT: " + std::to_string(_colorBuffers.size()) + "]" : "") +
            (_colorBuffer ? "[Color]" : "") +
            (_depth ? "[Depth]" : "") +
            (_stencil ? "[Stencil]" : "") +
            "[Face:" + std::to_string(_face) + "]");
    }

    RenderTarget::~RenderTarget()
    {
        spdlog::trace("DeAlloc: Id " + std::to_string(_id) + " " + _name);

        if (_device) {
            _device->removeTarget(this);

            if (_device->renderTarget().get() == this) {
                _device->setRenderTarget(nullptr);
            }
        }
    }

    int RenderTarget::width() const
    {
        auto width = _colorBuffer != nullptr ? _colorBuffer->width() : _depthBuffer != nullptr ? _depthBuffer->width() : _device->size().first;
        if (_mipLevel > 0) {
            width = TextureUtils::calcLevelDimension(width, _mipLevel);
        }
        return width;
    }

    int RenderTarget::height() const
    {
        int height = _colorBuffer ? _colorBuffer->height() : (_depthBuffer ? _depthBuffer->height() : _device->size().second);
        if (_mipLevel > 0) {
            height = TextureUtils::calcLevelDimension(height, _mipLevel);
        }
        return height;
    }

    void RenderTarget::resize(int width, int height)
    {
        if (this->width() != width || this->height() != height) {
            if (_mipLevel > 0) {
                spdlog::warn("Only a render target rendering to mipLevel 0 can be resized, ignoring.");
                return;
            }

            // Release existing
            destroyFrameBuffers();
            if (_device->renderTarget().get() == this) {
                _device->setRenderTarget(nullptr);
            }

            // Resize textures
            if (_depthBuffer) {
                _depthBuffer->resize(width, height);
            }
            for (auto* colorBuffer : _colorBuffers) {
                if (colorBuffer) {
                    colorBuffer->resize(width, height);
                }
            }

            // Initialize again
            validateMrt();
            createFrameBuffers();
        }
    }

    Texture* RenderTarget::getColorBuffer(size_t index) const
    {
        if (index < _colorBuffers.size()) {
            return _colorBuffers[index];
        }
        return nullptr;
    }

    void RenderTarget::validateMrt()
    {
        if (!_colorBuffers.empty()) {
            int width = _colorBuffers[0]->width();
            int height = _colorBuffers[0]->height();
            bool cubemap = _colorBuffers[0]->isCubemap();
            bool volume = _colorBuffers[0]->isVolume();

            for (size_t i = 1; i < _colorBuffers.size(); i++) {
                Texture* colorBuffer = _colorBuffers[i];
                assert(colorBuffer->width() == width && "All render target color buffers must have the same width");
                assert(colorBuffer->height() == height &&
                    "All render target color buffers must have the same height");
                assert(colorBuffer->isCubemap() == cubemap &&
                    "All render target color buffers must have the same cubemap setting");
                assert(colorBuffer->isVolume() == volume &&
                    "All render target color buffers must have the same volume setting");
            }
        }
    }
}
