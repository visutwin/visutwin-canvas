// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.11.2026
//
// The hierarchical depth (Hi-Z) pyramid of a camera's scene depth: an RG32F mip chain whose
// texels hold the nearest (r) and the farthest (g) depth buffer value of the depth texels they
// cover (the layout is described with HiZMapHandle, sceneMapHandles.h). It is the input a GPU
// occlusion cull tests an instance's projected bounds against: an instance whose nearest depth
// lies beyond the farthest depth of every pyramid texel its rectangle covers is hidden.
//
// Each level is rendered by the Slang program `hiz-pyramid` from the level above, reading
// texels by coordinate so every value is exact: level 0 from the depth map (a depth texture,
// or an R32F texture holding depth in red), level k from level k - 1. A texture cannot be
// sampled while one of its levels is the attachment, so a level k > 0 is rendered into a
// scratch texture (the size of level 1) and copied back into the chain by a second quad: one
// pass for level 0, two for each level after it. HiZPyramidGenerator does that wherever the
// caller stands between device passes; RenderPassHiZ is the same work as a pass of the frame
// graph, owned by a camera that asked for it (CameraComponent::requestHiZ) and run by the
// forward renderer after everything the camera renders, which publishes the pyramid to the
// camera's handle for a compute kernel (Compute::setHiZMap, the `vtscenemaps` module).
//
#pragma once

#include <memory>
#include <vector>

#include "platform/graphics/renderPass.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class RenderTarget;
    class Texture;
    struct HiZMapHandle;
    struct SceneDepthMapHandle;

    class HiZPyramidGenerator
    {
    public:
        explicit HiZPyramidGenerator(const std::shared_ptr<GraphicsDevice>& device);
        ~HiZPyramidGenerator();

        /// Builds the pyramid from `source`: a depth texture (DEPTH, DEPTH16 or DEPTHSTENCIL) or
        /// an R32F texture holding depth in red. Must run between device passes (no render pass
        /// open) inside a frame or an offline scope. The pyramid is reallocated when the source's
        /// size changes. Returns it, or null when the source cannot be read.
        std::shared_ptr<Texture> generate(Texture* source);

        /// The pyramid built last, or null.
        const std::shared_ptr<Texture>& pyramid() const { return _pyramid; }

        /// Creates the shaders ahead of the first use (RenderPass::prepareShaders).
        void prepareShaders();

        /// The size of pyramid level `level` along an axis whose depth map has `depthSize`
        /// texels: level 0 is half the depth (rounded down, at least 1), each level half the one
        /// above.
        static uint32_t levelSize(uint32_t depthSize, uint32_t level);

    private:
        bool allocate(uint32_t depthWidth, uint32_t depthHeight);

        class QuadPass;

        std::shared_ptr<GraphicsDevice> _device;
        std::unique_ptr<QuadPass> _depthPass;
        std::unique_ptr<QuadPass> _floatPass;
        std::unique_ptr<QuadPass> _reducePass;
        std::unique_ptr<QuadPass> _copyPass;
        std::shared_ptr<Texture> _pyramid;
        std::vector<std::shared_ptr<RenderTarget>> _levelTargets;
        std::shared_ptr<Texture> _scratch;
        std::shared_ptr<RenderTarget> _scratchTarget;
        uint32_t _depthWidth = 0;
        uint32_t _depthHeight = 0;
        bool _warnedSource = false;
    };

    /**
     * A camera's Hi-Z pyramid as a pass of the frame graph: no target of its own (so it never
     * merges); its execute() builds the pyramid from the depth `depth` names, when that depth
     * was rendered in this frame, and publishes it to `hiZ`. A frame whose depth was not
     * rendered leaves the handle naming the last pyramid, with its render version.
     */
    class RenderPassHiZ : public RenderPass
    {
    public:
        RenderPassHiZ(const std::shared_ptr<GraphicsDevice>& device, std::shared_ptr<SceneDepthMapHandle> depth,
            std::shared_ptr<HiZMapHandle> hiZ);

        void prepareShaders() override;
        void execute() override;

        const HiZPyramidGenerator& generator() const { return _generator; }

    private:
        std::shared_ptr<SceneDepthMapHandle> _depth;
        std::shared_ptr<HiZMapHandle> _hiZ;
        HiZPyramidGenerator _generator;
    };
}
