// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.11.2026
//
// The Gaussian mipmap filter (MipmapFilter::MIPMAPFILTER_GAUSSIAN) for the content RENDERED
// into a 2D texture: each level is rendered from the level above it by the Slang program
// `mipmap-gaussian` (13 bilinear taps approximating a Gaussian blur over about 6x6 texels),
// where the backends' own generation averages 2x2 blocks. The small levels a rough planar
// reflection or a frosted refraction samples then stay smooth; with the box chain they show
// blocks that flicker as the scene moves.
//
// A texture cannot be sampled while one of its levels is the attachment, so a level is rendered
// into a scratch texture of the same format (half the size, kept per format and grown to the
// largest texture it served) and copied back into the texture's chain by a second quad: two
// small passes per level. GaussianMipmapGenerator does that for one texture, wherever the
// caller stands between device passes (the colour grab's execute); RenderPassGaussianMipmaps
// is the same work as a pass of the frame graph, which the forward renderer runs after each
// block rendering a target whose colour buffer asks for the filter (Texture::gaussianMipmaps).
// Both backends then leave that texture's chain alone at the end of its pass.
//
#pragma once

#include <array>
#include <map>
#include <memory>
#include <vector>

#include "platform/graphics/constants.h"
#include "platform/graphics/renderPass.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class RenderPassShaderQuad;
    class RenderTarget;
    class Texture;

    class GaussianMipmapGenerator
    {
    public:
        explicit GaussianMipmapGenerator(const std::shared_ptr<GraphicsDevice>& device);
        ~GaussianMipmapGenerator();

        /// Renders every level of `texture` below its top one, with the Gaussian filter. Must
        /// run between device passes (no render pass open). A texture that is not a mipmapped
        /// 2D one asking for the filter is left alone; returns whether the levels were rendered.
        bool generate(Texture* texture);

        /// Creates the render targets over the texture's levels (and the scratch texture)
        /// ahead of generate(). The first render target over a texture created empty recreates
        /// its GPU image, so an owner that copies into the texture calls this BEFORE the copy,
        /// or the copy is lost with the image. Returns whether the texture takes the filter.
        bool prepare(Texture* texture);

        /// Creates the shaders ahead of the first use (RenderPass::prepareShaders).
        void prepareShaders();

    private:
        struct Scratch
        {
            std::shared_ptr<Texture> texture;
            // one target per level of the scratch texture
            std::vector<std::shared_ptr<RenderTarget>> targets;
        };

        Scratch& scratchFor(const Texture* texture);
        std::shared_ptr<RenderTarget> levelTarget(Texture* texture, uint32_t level);

        class QuadPass;

        std::shared_ptr<GraphicsDevice> _device;
        std::unique_ptr<QuadPass> _blurPass;
        std::unique_ptr<QuadPass> _copyPass;
        std::map<PixelFormat, Scratch> _scratch;
        // The targets over the levels of the textures served, keyed by texture: an owner serves
        // the textures it holds, so a pointer here outlives no texture it names.
        struct LevelTargets
        {
            const Texture* texture = nullptr;
            uint32_t width = 0;
            uint32_t height = 0;
            std::vector<std::shared_ptr<RenderTarget>> targets;
        };
        std::vector<LevelTargets> _levelTargets;
    };

    /**
     * The Gaussian mipmap generation of one texture as a pass of the frame graph: no target of
     * its own (so it never merges), its execute() renders the levels. The forward renderer keeps
     * one per texture it serves and adds it after each block rendering that texture.
     */
    class RenderPassGaussianMipmaps : public RenderPass
    {
    public:
        RenderPassGaussianMipmaps(const std::shared_ptr<GraphicsDevice>& device,
            const std::weak_ptr<RenderTarget>& target);

        /// The target whose first colour buffer it serves; null once that target is gone.
        std::shared_ptr<RenderTarget> target() const { return _target.lock(); }
        Texture* texture() const;

        void prepareShaders() override;
        void execute() override;

    private:
        std::weak_ptr<RenderTarget> _target;
        GaussianMipmapGenerator _generator;
    };
}
