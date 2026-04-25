// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#pragma once

#include <memory>
#include <vector>

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Texture;

    struct EnvReprojectRect
    {
        int rectX = 0;
        int rectY = 0;
        int rectW = 0;
        int rectH = 0;
        int seamPixels = 1;
    };

    struct EnvReprojectOptions
    {
        std::shared_ptr<Texture> source;
        bool sourceIsCubemap = false;
        std::shared_ptr<Texture> target;
        std::vector<EnvReprojectRect> rects;
        bool encodeRgbp = true;
        bool decodeSrgb = false;
    };

    void reprojectTexture(GraphicsDevice* device, const EnvReprojectOptions& options);

    // samples: numSamples × 4 floats (tangentX, tangentY, tangentZ, mipLevel).
    // Must remain valid for the duration of the convolveTexture() call.
    struct EnvConvolveRect
    {
        int rectX = 0;
        int rectY = 0;
        int rectW = 0;
        int rectH = 0;
        int seamPixels = 1;
        const float* samples = nullptr;
        int numSamples = 0;
        // NoL-weighted accumulation (sum * L.z) / sum(L.z): required for GGX
        // specular prefilter. Lambert / Phong leave this false for a uniform
        // average. Mirrors PlayCanvas prefilterSamples vs. prefilterSamplesUnweighted.
        bool weightByNoL = false;
    };

    struct EnvConvolveOptions
    {
        std::shared_ptr<Texture> source;
        bool sourceIsCubemap = false;
        std::shared_ptr<Texture> target;
        std::vector<EnvConvolveRect> rects;
        bool encodeRgbp = true;
        bool decodeSrgb = false;
    };

    void convolveTexture(GraphicsDevice* device, const EnvConvolveOptions& options);

    struct EnvAtlasBakeOptions
    {
        std::shared_ptr<Texture> target;

        std::shared_ptr<Texture> reprojectSource;
        bool reprojectSourceIsCubemap = false;
        std::vector<EnvReprojectRect> reprojectRects;

        std::shared_ptr<Texture> convolveSource;
        bool convolveSourceIsCubemap = false;
        std::vector<EnvConvolveRect> convolveRects;

        bool encodeRgbp = true;
        bool decodeSrgb = false;
    };

    void bakeEnvAtlas(GraphicsDevice* device, const EnvAtlasBakeOptions& options);

    // Builds a mipmapped HDR cubemap from an equirectangular source. Returned
    // texture is RGBA32F with a full mip chain generated via blit.
    std::shared_ptr<Texture> equirectToCubemap(GraphicsDevice* device,
        const std::shared_ptr<Texture>& source, int faceSize, bool decodeSrgb = false);
}
