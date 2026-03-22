// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.09.2025.
//
#pragma once

#include <unordered_set>

#include "texture.h"

namespace visutwin::canvas
{
    struct RenderTargetOptions
    {
        GraphicsDevice* graphicsDevice = nullptr;

        Texture* colorBuffer = nullptr;

        std::vector<Texture*> colorBuffers;

        Texture* depthBuffer = nullptr;

        bool depth = false;

        int face = 0;

        int samples = 1;

        bool stencil = false;

        bool autoResolve = false;

        std::string name;

        int mipLevel = 0;

        bool flipY;
    };

    /*
     * A render target is a rectangular rendering surface
     */
    class RenderTarget
    {
    public:
        explicit RenderTarget(const RenderTargetOptions& options = {});

        virtual ~RenderTarget();

        // Width of the render target in pixels
        int width() const;

        int height() const;

        void resize(int width, int height);

        Texture* colorBuffer() const { return _colorBuffer; }

        int samples() const { return _samples; }

        bool hasDepthBuffer() const { return _depthBuffer != nullptr; }
        bool hasDepth() const { return _depth; }

        int colorBufferCount() const { return _colorBuffers.size(); }

        bool hasMipmaps() const { return _mipmaps; }

        Texture* getColorBuffer(size_t index) const;

        bool hasStencil() const { return _stencil; }

        int mipLevel() const { return _mipLevel; }

        const std::string& name() const { return _name; }

        Texture* depthBuffer() const { return _depthBuffer; }
        bool autoResolve() const { return _autoResolve; }

        // Cubemap face index (0-5). Used when rendering to a specific face of a cubemap texture.
        int face() const { return _face; }

        int key() const { return _id; }

    protected:
        GraphicsDevice* device() const { return _device; }

        // Validates that all MRT color buffers have the same dimensions and settings
        void validateMrt();

        virtual void destroyFrameBuffers() = 0;
        virtual void createFrameBuffers() = 0;

    private:
        GraphicsDevice* _device;

        int _mipLevel;

        Texture* _colorBuffer = nullptr;

        std::vector<Texture*> _colorBuffers;

        Texture* _depthBuffer = nullptr;

        int _samples = 1;

        bool _mipmaps = true;

        bool _stencil;

        std::string _name;

        int _id;

        bool _depth;

        int _face;

        bool _autoResolve;

        bool _flipY;
    };
}
