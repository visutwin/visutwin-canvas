// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 17.10.2026
//
// The noise textures shaping the clouds of DynamicSkyEffect, generated on the CPU when first
// used and shared by the effects of a device:
//   cloudMap   256^2 RGBA8, the flat clouds and the volumetric clouds' weather: r fractal noise
//              of the large shapes, g cellular noise rounding them into puffs, b fractal noise
//              (equalized), a the weather gathering the clouds into fields (equalized);
//   shapeMap   64^3 R8 Perlin-Worley noise, the shapes of the volumetric clouds;
//   detailMap  32^3 R8 fractal cellular noise eroding their edges.
// The 3D noise lives in the layers of 2D texture arrays (the shader interpolates between the
// layers) and is equalized by its cumulative histogram, so a threshold covers exactly its
// fraction of the volume. The generators reproduce upstream's arithmetic (its 32-bit integer
// hashes, its float32 storage and its stable sort), so the noise is the same.
//
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Texture;

    namespace dynamic_sky_noise
    {
        /// The CPU halves, exposed for tests: an RGBA8 tileable 2D noise of `size`^2 and an R8
        /// 3D noise of `size`^3 (layer-major, z slowest), as stored.
        std::vector<uint8_t> cloudMapData(int size = 256);
        std::vector<uint8_t> shapeMapData(int size = 64);
        std::vector<uint8_t> detailMapData(int size = 32);

        /// Upstream's 2D lattice hash, to 0-1.
        double hash2(int x, int y, int seed);
    }

    class DynamicSkyNoise
    {
    public:
        explicit DynamicSkyNoise(GraphicsDevice* device) : _device(device) {}
        ~DynamicSkyNoise();

        /// The noise of a device, shared while any effect holds it; created on first use.
        static std::shared_ptr<DynamicSkyNoise> forDevice(GraphicsDevice* device);

        Texture* cloudMap();
        Texture* shapeMap();
        Texture* detailMap();
        /// The 32x32 blue noise tile (RGBA8, nearest, repeating) the volumetric march jitters by.
        Texture* blueNoise();

    private:
        GraphicsDevice* _device = nullptr;
        std::shared_ptr<Texture> _cloudMap;
        std::shared_ptr<Texture> _shapeMap;
        std::shared_ptr<Texture> _detailMap;
        std::shared_ptr<Texture> _blueNoise;
    };
}
