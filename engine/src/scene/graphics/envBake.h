// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Environment bakes as backend-agnostic code over QuadRender, replacing the
// per-backend pass classes and their GraphicsDevice virtuals.
//
// These run OUTSIDE the frame loop, so each entry point opens a
// GraphicsDevice::beginOfflineWork scope; inside it the ordinary render-pass and
// draw API is usable on both backends.
//
#pragma once

#include <vector>

#include "platform/graphics/constants.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Texture;

    /// One destination rectangle of a reprojection. `seamPixels` is the border
    /// baked around the rect so a bilinear tap at its edge stays inside it.
    struct EnvBakeRect
    {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        int seamPixels = 0;
    };

    struct EnvReprojectRequest
    {
        Texture* target = nullptr;
        Texture* source = nullptr;   // 2D or cubemap; the projection says which
        std::vector<EnvBakeRect> rects;
        TextureProjection sourceProjection = TextureProjection::TEXTUREPROJECTION_EQUIRECT;
        TextureProjection targetProjection = TextureProjection::TEXTUREPROJECTION_EQUIRECT;
        bool encodeRgbp = true;
        bool decodeSrgb = false;
    };

    /**
     * Fill each of `target`'s six cube faces by sampling `source` along the
     * direction that face's texels look at, then generate its mip chain.
     * `target` must already be created as a cubemap. Returns false if the
     * arguments or the shader are unusable, leaving the target untouched.
     */
    bool bakeEquirectToCubemap(GraphicsDevice* device, Texture* source,
        Texture* target, bool decodeSrgb);

    /**
     * Resample `source` into each of `request.rects` on `target`, converting
     * between equirect, octahedral and cubemap layouts. Every rect is drawn
     * inside ONE render pass: on Apple-Silicon tile GPUs a load-and-scissor pass
     * does not reliably preserve content outside the scissor, so splitting the
     * rects across passes loses everything but the last.
     */
    bool bakeReproject(GraphicsDevice* device, const EnvReprojectRequest& request);

    /// One convolution rectangle. `samples` is `numSamples` float4s of
    /// (tangentX, tangentY, tangentZ, mipLevel): the hemisphere direction for
    /// Lambert, the reflected direction for GGX. A sample with a non-positive z is
    /// skipped by the shader. The table is uploaded as an RGBA32F data texture.
    struct EnvConvolveBakeRect
    {
        EnvBakeRect rect;
        const float* samples = nullptr;
        int numSamples = 0;
        bool weightByNoL = false;
    };

    struct EnvConvolveRequest
    {
        Texture* target = nullptr;
        Texture* source = nullptr;
        std::vector<EnvConvolveBakeRect> rects;
        bool encodeRgbp = true;
        bool decodeSrgb = false;
    };

    /// Importance-sampled convolution of `source` into `target`'s rects.
    bool bakeConvolve(GraphicsDevice* device, const EnvConvolveRequest& request);

    /// The combined atlas bake: the reproject rects first, then the convolve rects,
    /// all inside ONE render pass. That single pass is required, not an
    /// optimisation — on Apple-Silicon tile GPUs a load-and-scissor pass does not
    /// preserve content outside its scissor, so splitting the atlas across passes
    /// keeps only the last rect. The two sources differ in practice: reprojection
    /// resamples the environment while convolution reads a mipped cubemap.
    struct EnvAtlasRequest
    {
        Texture* target = nullptr;
        Texture* reprojectSource = nullptr;
        TextureProjection reprojectSourceProjection = TextureProjection::TEXTUREPROJECTION_EQUIRECT;
        TextureProjection reprojectTargetProjection = TextureProjection::TEXTUREPROJECTION_EQUIRECT;
        std::vector<EnvBakeRect> reprojectRects;
        Texture* convolveSource = nullptr;
        std::vector<EnvConvolveBakeRect> convolveRects;
        bool encodeRgbp = true;
        bool decodeSrgb = false;
    };

    bool bakeEnvAtlas(GraphicsDevice* device, const EnvAtlasRequest& request);
}
