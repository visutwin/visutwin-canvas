// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#pragma once

#include <memory>
#include <vector>

#include "renderTarget.h"
#include "core/math/color.h"

namespace visutwin::canvas
{
    struct RenderPassOptions
    {
        float scaleX = 1.0f;

        float scaleY = 1.0f;

        std::shared_ptr<Texture> resizeSource;
    };

    /**
     * Color attachment operations specify how color attachments are handled at the beginning and end of a render pass
     */
    class ColorAttachmentOps {
    public:
        Color clearValue{0.0f, 0.0f, 0.0f, 1.0f};        // Color to clear to (sRGB space)
        Color clearValueLinear{0.0f, 0.0f, 0.0f, 1.0f};  // Color to clear to (linear space)
        bool clear = false;                               // Clear before rendering
        bool store = false;                               // Store after render pass
        bool resolve = true;                              // Resolve multi-sampled surface
        bool genMipmaps = false;                          // Generate mipmaps after rendering

        ColorAttachmentOps() = default;
    };

    /**
     * Depth-stencil attachment operations specify how depth and stencil attachments
     * are handled at the beginning and end of a render pass
     */
    class DepthStencilAttachmentOps {
    public:
        float clearDepthValue = 1.0f;    // Depth value to clear to
        int clearStencilValue = 0;       // Stencil value to clear to
        bool clearDepth = false;         // Clear depth before rendering
        bool clearStencil = false;       // Clear stencil before rendering
        bool storeDepth = false;         // Store depth after render pass
        bool resolveDepth = false;       // Resolve multi-sampled depth
        bool storeStencil = false;       // Store stencil after render pass

        DepthStencilAttachmentOps() = default;
    };

    class GraphicsDevice;

    /*
    * A render pass represents a node in the frame graph and encapsulates a system which
    * renders to a render target using an execution callback
    */
    class RenderPass
    {
    public:
        RenderPass(const std::shared_ptr<GraphicsDevice>& device) : _device(device) {};

        virtual void init(const std::shared_ptr<RenderTarget>& renderTarget = nullptr,
            const std::shared_ptr<RenderPassOptions>& options = nullptr);

        virtual void frameUpdate() const;

        virtual void before() {}
        virtual void after() {}
        virtual void execute() {}

        void render();

        float scaleX() const { return _options ? _options->scaleX : 1.0f; }

        float scaleY() const { return _options ? _options->scaleY : 1.0f; }

        const std::vector<std::shared_ptr<RenderPass>>& beforePasses() const { return _beforePasses; }

        const std::vector<std::shared_ptr<RenderPass>>& afterPasses() const { return _afterPasses; }
        void addBeforePass(const std::shared_ptr<RenderPass>& renderPass);
        void addAfterPass(const std::shared_ptr<RenderPass>& renderPass);
        void clearBeforePasses();
        void clearAfterPasses();

        void setClearColor(const Color* color = nullptr);
        void setClearDepth(const float* depthValue = nullptr);
        void setClearStencil(const int* stencilValue = nullptr);

        bool enabled() const { return _enabled; }
        void setEnabled(bool value);

        std::shared_ptr<RenderTarget> renderTarget() const { return _renderTarget; };

        const std::vector<std::shared_ptr<ColorAttachmentOps>>& colorArrayOps() const { return _colorArrayOps; }

        std::shared_ptr<DepthStencilAttachmentOps> depthStencilOps() const { return _depthStencilOps; }

        void setSkipStart(const bool value) { _skipStart = value; }
        void setSkipEnd(const bool value) { _skipEnd = value; }

        bool requiresCubemaps() const { return _requiresCubemaps; }
        void setRequiresCubemaps(bool value) { _requiresCubemaps = value; }

        virtual void onEnable() {}
        virtual void onDisable() {}

        void setOptions(const std::shared_ptr<RenderPassOptions>& value);

        void allocateAttachments();

        virtual void postInit() {}

        void log(std::shared_ptr<GraphicsDevice> device, int index = 0) const;

        std::shared_ptr<ColorAttachmentOps> colorOps() const;

    protected:
        std::shared_ptr<GraphicsDevice> device() const { return _device; }

        bool _requiresCubemaps = true;

        std::string _name;

    private:
        std::shared_ptr<GraphicsDevice> _device;

        std::shared_ptr<RenderPassOptions> _options;

        std::shared_ptr<RenderTarget> _renderTarget;
        bool _renderTargetInitialized = false;

        // Render passes which need to be executed before this pass
        std::vector<std::shared_ptr<RenderPass>> _beforePasses;
        std::vector<std::shared_ptr<RenderPass>> _afterPasses;

        bool _enabled = true;

        bool _executeEnabled = true;

        bool _skipStart = false;
        bool _skipEnd = false;

        std::vector<std::shared_ptr<ColorAttachmentOps>> _colorArrayOps;
        std::shared_ptr<DepthStencilAttachmentOps> _depthStencilOps;

        int _samples = 0;
    };
}
