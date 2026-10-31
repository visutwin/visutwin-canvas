// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.09.2025
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

        /// The cubemap face (0-5), the array texture LAYER or the volume DEPTH SLICE the colour
        /// (and a layered depth) buffer is rendered into. A volume slice is never multisampled.
        int face = 0;

        int samples = 1;

        bool stencil = false;

        /// A multisampled target resolves into its colour (and depth) buffers at the end of each
        /// pass. On by default: off, the buffers hold nothing a pass rendered (an MSAA bake into a
        /// texture array came out as uninitialised memory). Meaningless at one sample.
        bool autoResolve = true;
        /// The multisampled twins are TRANSIENT: rendered and resolved within one
        /// pass, never stored for a later pass to load. Metal then keeps them in
        /// tile memory (StorageModeMemoryless); a pass that nevertheless asks to
        /// store one is downgraded to a resolve with a warning. Leave false for a
        /// target a later pass renders into without clearing.
        bool transientMultisample = false;

        /// Over MULTISAMPLED textures (TextureOptions::samples): the single-sampled texture the
        /// colour resolves into at the end of each pass, by the hardware (an average). Without one
        /// the samples are only stored, for a shader to read. Ignored over single-sampled colour.
        Texture* resolveBuffer = nullptr;
        /// Over a MULTISAMPLED depth texture: a single-sampled R32F texture the depth is resolved
        /// into after each pass that renders the target, by depthResolveMode, by a shader pass the
        /// renderer adds (ForwardRenderer).
        Texture* depthResolveBuffer = nullptr;
        DepthResolveMode depthResolveMode = DepthResolveMode::DEPTHRESOLVE_SAMPLE0;

        std::string name;

        int mipLevel = 0;

        // DEVIATION: no `origin` / `flipY`. Upstream needs them because WebGL stores
        // a render target bottom-up; here row 0 is the TOP row on both backends (Metal
        // natively, Vulkan through its negated-height viewport), the same as a loaded
        // image, and the only origin.
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

        /// Rebuilds the GPU-side attachments over the current textures: for a target that
        /// shares a texture another target resized, whose own size already reads the new one
        /// so resize() would do nothing.
        void refreshAttachments();

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
        /// True when the colour (or depth) buffers ARE multisampled textures, rendered into
        /// directly: the pass stores them, and resolves the colour only into resolveBuffer.
        bool multisampledTextures() const { return _multisampledTextures; }
        Texture* resolveBuffer() const { return _resolveBuffer; }
        Texture* depthResolveBuffer() const { return _depthResolveBuffer; }
        DepthResolveMode depthResolveMode() const { return _depthResolveMode; }
        /// The depth resolve mode may change at any time; the next resolve takes it.
        void setDepthResolveMode(const DepthResolveMode mode) { _depthResolveMode = mode; }
        bool autoResolve() const { return _autoResolve; }
        bool transientMultisample() const { return _transientMultisample; }

        // The cubemap face (0-5), array layer or volume depth slice the target renders into.
        int face() const { return _face; }

        int key() const { return _id; }

    protected:
        /// Null once the device has been torn down (detachFromDevice).
        GraphicsDevice* device() const { return _device; }

        // Lower the sample count after construction. A backend calls this when
        // the multisampled surfaces could not be created (an attachment format
        // with no MSAA support, say); everything downstream — the pass's cached
        // sample count and the pipeline it is keyed on — reads samples(), so the
        // fallback has to be visible there rather than kept in the backend.
        void setSamples(const int value) { _samples = value > 1 ? value : 1; }

        // Validates that all MRT color buffers have the same dimensions and settings
        void validateMrt();

        virtual void destroyFrameBuffers() = 0;
        virtual void createFrameBuffers() = 0;

    private:
        // The device detaches every render target still alive when it is torn down.
        friend class GraphicsDevice;

        /// Called by the device while it is torn down: releases the backend's attachments
        /// while the device can still free them and forgets the device. The target keeps its
        /// options and attachment pointers, so it can be destroyed at any time later; with
        /// no device it no longer resizes, and its size falls back to 0 where it would ask
        /// the device.
        void detachFromDevice();

        GraphicsDevice* _device;

        int _mipLevel;

        Texture* _colorBuffer = nullptr;

        std::vector<Texture*> _colorBuffers;

        Texture* _depthBuffer = nullptr;
        Texture* _resolveBuffer = nullptr;
        Texture* _depthResolveBuffer = nullptr;
        DepthResolveMode _depthResolveMode = DepthResolveMode::DEPTHRESOLVE_SAMPLE0;
        bool _multisampledTextures = false;

        int _samples = 1;

        bool _mipmaps = true;

        bool _stencil;

        std::string _name;

        int _id;

        bool _depth;

        int _face;

        bool _autoResolve;
        bool _transientMultisample = false;
    };
}
