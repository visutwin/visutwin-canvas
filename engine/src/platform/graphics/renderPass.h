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
        // Passes are held and released through base pointers (the frame graph, a
        // parent's before/after lists), so the destructor must dispatch.
        virtual ~RenderPass() = default;

        virtual void init(const std::shared_ptr<RenderTarget>& renderTarget = nullptr,
            const std::shared_ptr<RenderPassOptions>& options = nullptr);

        virtual void frameUpdate() const;

        virtual void before() {}
        virtual void after() {}
        virtual void execute() {}

        void render();

        /** Debug name (set by subclasses) — used by the GPU profiler. */
        /// The pass's name for the profiler and the HUD: what the pass set, or its
        /// class name when it set nothing — so the quad passes stop collapsing into
        /// one anonymous "pass" row wherever timings are keyed by name.
        const std::string& name() const;

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
        bool skipStart() const { return _skipStart; }
        bool skipEnd() const { return _skipEnd; }

        /// FrameGraph::compile's edits to this pass's attachment flags (a store it
        /// raised because a later pass loads the target, a mipmap generation it dropped
        /// for a cubemap face). Passes persist across frames while the graph is rebuilt
        /// every frame, so each edit is recorded — the flag, the value it replaced and
        /// the value written — and undone at the next compile before the graph derives
        /// them afresh. Only a flag still holding the graph's value is put back: a pass
        /// that changed its own flag since keeps its change. Until 2026-09-25 the edits
        /// were one-way, so one frame's adjacency stuck to a pass for good.
        void setAttachmentFlagByGraph(const std::shared_ptr<void>& owner, bool& flag, bool value);
        void undoGraphAttachmentEdits();

        bool requiresCubemaps() const { return _requiresCubemaps; }
        void setRequiresCubemaps(bool value) { _requiresCubemaps = value; }

        /// True when this pass READS its depth attachment without writing it — a
        /// fullscreen effect that composites using scene depth while keeping that
        /// depth attached for later passes. Vulkan permits sampling an attachment
        /// only when it is bound read-only, so this makes the backend choose
        /// DEPTH_STENCIL_READ_ONLY_OPTIMAL for the attachment and, through the
        /// texture's tracked layout, for the descriptor that samples it. The depth
        /// ops cannot express this: such a pass sets storeDepth to preserve the
        /// contents, which is indistinguishable from writing them.
        bool depthReadOnly() const { return _depthReadOnly; }
        void setDepthReadOnly(const bool value) { _depthReadOnly = value; }

        virtual void onEnable() {}
        virtual void onDisable() {}

        void setOptions(const std::shared_ptr<RenderPassOptions>& value);

        void allocateAttachments();

        virtual void postInit() {}

        std::shared_ptr<ColorAttachmentOps> colorOps() const;

    protected:
        std::shared_ptr<GraphicsDevice> device() const { return _device; }

        bool _requiresCubemaps = true;
        bool _depthReadOnly = false;

        std::string _name;
        mutable std::string _resolvedName;   // name() cache when _name is empty

        /**
         * True when render() deliberately skipped execute() for the current
         * pass — either because execution is disabled, or because the device
         * could not begin the pass (a Vulkan frame skipped at swapchain
         * acquire, for instance). after() still runs in that case, so
         * subclasses doing lifecycle parity checks must treat a skipped
         * execute as legitimate rather than as a missed call.
         */
        bool executeSkipped() const { return _executeSkipped; }

    private:
        std::shared_ptr<GraphicsDevice> _device;

        bool _executeSkipped = false;

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

        struct GraphEdit
        {
            std::shared_ptr<void> owner;   // keeps the ops object the flag lives in alive
            bool* flag = nullptr;
            bool previous = false;
            bool written = false;
        };
        std::vector<GraphEdit> _graphEdits;

        std::vector<std::shared_ptr<ColorAttachmentOps>> _colorArrayOps;
        std::shared_ptr<DepthStencilAttachmentOps> _depthStencilOps;

        int _samples = 0;
    };
}
