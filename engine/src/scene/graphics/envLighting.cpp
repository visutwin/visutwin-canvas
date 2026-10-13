// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// CPU-based environment lighting atlas generation.
//
#include "envLighting.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <tuple>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "core/math/random.h"
#include "core/math/vector3.h"
#include "envBake.h"
#include "envReproject.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
    // ----------------------------------------------------------------
    // Constants
    // ----------------------------------------------------------------
    static constexpr float ENV_PI = std::numbers::pi_v<float>;
    static constexpr float ENV_TWO_PI = 2.0f * ENV_PI;

    static int calcLevels(int size)
    {
        return 1 + static_cast<int>(std::floor(std::log2(std::max(size, 1))));
    }

    // ----------------------------------------------------------------
    // Required samples table for GGX (pre-calculated)
    // ----------------------------------------------------------------
    int EnvLighting::getRequiredSamplesGGX(int numSamples, int specularPower)
    {
        static const std::unordered_map<int, std::unordered_map<int, int>> table = {
            {16, {{2, 26}, {8, 20}, {32, 17}, {128, 16}, {512, 16}}},
            {32, {{2, 53}, {8, 40}, {32, 34}, {128, 32}, {512, 32}}},
            {128, {{2, 214}, {8, 163}, {32, 139}, {128, 130}, {512, 128}}},
            {1024, {{2, 1722}, {8, 1310}, {32, 1114}, {128, 1041}, {512, 1025}}}
        };

        auto it = table.find(numSamples);
        if (it != table.end()) {
            auto it2 = it->second.find(specularPower);
            if (it2 != it->second.end()) {
                return it2->second;
            }
        }
        return numSamples;
    }

    // ----------------------------------------------------------------
    // Hemisphere sampling
    // ----------------------------------------------------------------
    void EnvLighting::hemisphereSampleGGX(float& hx, float& hy, float& hz, float xi1, float xi2, float a)
    {
        const float phi = xi2 * ENV_TWO_PI;
        const float cosTheta = std::sqrt((1.0f - xi1) / (1.0f + (a * a - 1.0f) * xi1));
        const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
        hx = std::cos(phi) * sinTheta;
        hy = std::sin(phi) * sinTheta;
        hz = cosTheta;
        // Normalize
        const float len = std::sqrt(hx * hx + hy * hy + hz * hz);
        if (len > 0.0f) {
            hx /= len;
            hy /= len;
            hz /= len;
        }
    }

    void EnvLighting::hemisphereSampleLambert(float& hx, float& hy, float& hz, float xi1, float xi2)
    {
        const float phi = xi2 * ENV_TWO_PI;
        const float cosTheta = std::sqrt(1.0f - xi1);
        const float sinTheta = std::sqrt(xi1);
        hx = std::cos(phi) * sinTheta;
        hy = std::sin(phi) * sinTheta;
        hz = cosTheta;
        // Normalize
        const float len = std::sqrt(hx * hx + hy * hy + hz * hz);
        if (len > 0.0f) {
            hx /= len;
            hy /= len;
            hz /= len;
        }
    }

    float EnvLighting::D_GGX(float NoH, float linearRoughness)
    {
        const float a = NoH * linearRoughness;
        const float k = linearRoughness / (1.0f - NoH * NoH + a * a);
        return k * k * (1.0f / ENV_PI);
    }


    // ----------------------------------------------------------------
    // GGX sample table used by the GPU convolve pass. Mirrors the sample
    // loop in writeGGXRegion (including requiredSamples / zero-padding for
    // below-hemisphere rejections).
    // ----------------------------------------------------------------
    std::vector<float> EnvLighting::generateGGXSampleTable(int numSamples, int specularPower, int sourceTotalPixels)
    {
        const float roughness = 1.0f - std::log2(static_cast<float>(std::max(specularPower, 1))) / 11.0f;
        const float a = roughness * roughness;
        const float pixelsPerSample = static_cast<float>(sourceTotalPixels)
            / static_cast<float>(std::max(numSamples, 1));
        const int requiredSamples = getRequiredSamplesGGX(numSamples, specularPower);

        std::vector<float> table;
        table.reserve(static_cast<size_t>(numSamples) * 4);
        int valid = 0;
        for (int i = 0; i < requiredSamples && valid < numSamples; ++i) {
            float hx, hy, hz;
            hemisphereSampleGGX(hx, hy, hz,
                static_cast<float>(i) / static_cast<float>(requiredSamples),
                Random::radicalInverse(i), a);

            // Reflection of N about H, with N = (0, 0, 1): L = 2 (N.H) H - N.
            const float NoH = hz;
            const Vector3 l = Vector3(hx, hy, hz) * (2.0f * NoH) - Vector3(0.0f, 0.0f, 1.0f);

            if (l.getZ() > 0.0f) {
                const float pdf = D_GGX(std::min(1.0f, NoH), a) / 4.0f + 0.001f;
                const float mip = 0.5f * std::log2(pixelsPerSample / pdf);
                const size_t base = table.size();
                table.resize(base + 4);
                l.store(&table[base]);
                table[base + 3] = mip;
                ++valid;
            }
        }
        while (valid < numSamples) {
            table.push_back(0.0f);
            table.push_back(0.0f);
            table.push_back(0.0f);
            table.push_back(0.0f);
            ++valid;
        }
        return table;
    }

    // ----------------------------------------------------------------
    // Lambert sample table used by the GPU convolve pass. Matches the sample
    // distribution + mip-level math of writeLambertRegion.
    // ----------------------------------------------------------------
    std::vector<float> EnvLighting::generateLambertSampleTable(int numSamples, int sourceTotalPixels)
    {
        const float pixelsPerSample = static_cast<float>(sourceTotalPixels)
            / static_cast<float>(std::max(numSamples, 1));

        std::vector<float> table;
        table.reserve(static_cast<size_t>(numSamples) * 4);
        for (int i = 0; i < numSamples; ++i) {
            float hx, hy, hz;
            hemisphereSampleLambert(hx, hy, hz,
                static_cast<float>(i) / static_cast<float>(numSamples),
                Random::radicalInverse(i));
            const float pdf = hz / ENV_PI;
            const float mip = 0.5f * std::log2(pixelsPerSample / std::max(pdf, 0.001f));
            table.push_back(hx);
            table.push_back(hy);
            table.push_back(hz);
            table.push_back(mip);
        }
        return table;
    }


    // ----------------------------------------------------------------
    // generateAtlas - GPU bake entry point.
    // ----------------------------------------------------------------
    Texture* EnvLighting::generateAtlas(GraphicsDevice* device, Texture* source,
                                        int size, int numReflectionSamples, int numAmbientSamples)
    {
        return generateAtlas(device, source, nullptr, size, numReflectionSamples, numAmbientSamples);
    }

    Texture* EnvLighting::generateAtlas(GraphicsDevice* device, Texture* source, Texture* target,
                                        int size, int numReflectionSamples, int numAmbientSamples,
                                        const bool decodeSrgb)
    {
        if (!device || !source) {
            spdlog::error("EnvLighting::generateAtlas: null device or source");
            return nullptr;
        }

        // Destination atlas — empty RGBP-encoded texture; the GPU bake fills it. An earlier
        // atlas of the same size is filled again instead.
        Texture* texture = target;
        if (!texture || texture->width() != static_cast<uint32_t>(size) ||
            texture->height() != static_cast<uint32_t>(size)) {
            spdlog::info("EnvLighting: generating {}x{} RGBP atlas from a {}x{} {} source",
                         size, size, source->width(), source->height(),
                         source->isCubemap() ? "cubemap" : "equirect");
            TextureOptions atlasOptions;
            atlasOptions.name = "envAtlas";
            atlasOptions.width = size;
            atlasOptions.height = size;
            atlasOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
            atlasOptions.mipmaps = false;
            atlasOptions.minFilter = FilterMode::FILTER_LINEAR;
            atlasOptions.magFilter = FilterMode::FILTER_LINEAR;
            texture = new Texture(device, atlasOptions);
            texture->setEncoding(TextureEncoding::RGBP);
            texture->upload();
        }

        // HDR cubemap for the GGX/Lambert convolution: the source itself when it is one
        // (decoded by the bake), else converted, and decoded once, from the equirect.
        std::shared_ptr<Texture> sourceShared(source, [](Texture*){});
        std::shared_ptr<Texture> cubemap = sourceShared;
        bool bakeDecodesSrgb = decodeSrgb;
        if (!source->isCubemap()) {
            bakeDecodesSrgb = false;
            cubemap = equirectToCubemap(device, sourceShared, 256, decodeSrgb);
            if (!cubemap) {
                spdlog::error("EnvLighting: failed to build HDR cubemap");
                return texture;
            }
        }
        const int cubemapSize = static_cast<int>(cubemap->width());

        // Layout: mipmap section along the top/diagonal, GGX along the left
        // column of the lower half, Lambert at its fixed sub-rect. Must match
        // the read-time math in the forward shaders.
        struct GGXLevel { int x, y, w, h, specularPower; };
        std::vector<GGXLevel> ggxLevels;
        {
            int rectX = 0, rectY = size / 2, rectW = size / 2, rectH = size / 4;
            for (int i = 1; i <= 6; ++i) {
                if (rectW < 1 || rectH < 1) break;
                const int specularPower = std::max(1, 2048 >> (i * 2));
                ggxLevels.push_back({rectX, rectY, rectW, rectH, specularPower});
                rectY += rectH;
                rectW = std::max(1, rectW / 2);
                rectH = std::max(1, rectH / 2);
            }
        }
        const int lambertRectX = size / 4;
        const int lambertRectY = size / 2 + size / 4;
        const int lambertRectW = size / 8;
        const int lambertRectH = size / 16;

        // Sample tables must outlive the bakeEnvAtlas call since EnvConvolveOp references
        // raw float*. They depend on their inputs alone (radical-inverse sequences), so
        // each is built once and kept: an atlas re-baked every frame would otherwise
        // rebuild the same thousands of samples every frame.
        const int sourceTotalPixels = cubemapSize * cubemapSize * 6;
        using SampleTable = std::shared_ptr<const std::vector<float>>;
        const auto cachedTable = [](const int specularPower, const int numSamples, const int totalPixels) {
            // specularPower 0 is the Lambert table.
            static std::mutex mutex;
            static std::map<std::tuple<int, int, int>, SampleTable> tables;
            const std::lock_guard lock(mutex);
            auto& table = tables[{specularPower, numSamples, totalPixels}];
            if (!table) {
                table = std::make_shared<const std::vector<float>>(specularPower > 0
                    ? generateGGXSampleTable(numSamples, specularPower, totalPixels)
                    : generateLambertSampleTable(numSamples, totalPixels));
            }
            return table;
        };
        std::vector<SampleTable> ggxSamples(ggxLevels.size());
        for (size_t i = 0; i < ggxLevels.size(); ++i) {
            ggxSamples[i] = cachedTable(ggxLevels[i].specularPower, numReflectionSamples, sourceTotalPixels);
        }
        const SampleTable lambertSamples = cachedTable(0, numAmbientSamples, sourceTotalPixels);

        EnvAtlasBakeOptions opts;
        opts.target = std::shared_ptr<Texture>(texture, [](Texture*){});
        opts.encodeRgbp = true;
        opts.decodeSrgb = bakeDecodesSrgb;

        // Sample the atlas mipmap (top half / diagonal) from the mipmapped HDR
        // cubemap intermediate, not the raw equirect. The cubemap has a full
        // mip chain so the hardware sampler picks the right LOD per dest rect;
        // sampling the 4K equirect into tiny rects without mip selection
        // aliases the smaller mips and dims high-roughness specular reflections
        // on curved surfaces.
        opts.reprojectSource = cubemap;
        opts.reprojectSourceIsCubemap = true;
        {
            const int levels = calcLevels(256) - calcLevels(4);
            int rectX = 0, rectY = 0, rectW = size, rectH = size / 2;
            for (int i = 0; i <= levels; ++i) {
                if (rectW < 1 || rectH < 1) break;
                EnvReprojectRect r;
                r.rectX = rectX; r.rectY = rectY; r.rectW = rectW; r.rectH = rectH;
                r.seamPixels = 1;
                opts.reprojectRects.push_back(r);
                rectX += rectH;
                rectY += rectH;
                rectW = std::max(1, rectW / 2);
                rectH = std::max(1, rectH / 2);
            }
        }

        opts.convolveSource = cubemap;
        opts.convolveSourceIsCubemap = true;
        for (size_t i = 0; i < ggxLevels.size(); ++i) {
            const auto& g = ggxLevels[i];
            EnvConvolveRect r;
            r.rectX = g.x; r.rectY = g.y; r.rectW = g.w; r.rectH = g.h;
            r.seamPixels = 1;
            r.samples = ggxSamples[i]->data();
            r.numSamples = numReflectionSamples;
            r.weightByNoL = true;
            opts.convolveRects.push_back(r);
        }
        {
            EnvConvolveRect r;
            r.rectX = lambertRectX; r.rectY = lambertRectY;
            r.rectW = lambertRectW; r.rectH = lambertRectH;
            r.seamPixels = 1;
            r.samples = lambertSamples->data();
            r.numSamples = numAmbientSamples;
            opts.convolveRects.push_back(r);
        }

        bakeEnvAtlas(device, opts);
        return texture;
    }

    // ----------------------------------------------------------------
    // generatePrefilteredAtlas
    // ----------------------------------------------------------------
    Texture* EnvLighting::generatePrefilteredAtlas(GraphicsDevice* device, const std::vector<Texture*>& sources,
        const int size, const int numAmbientSamples, const bool legacyAmbient)
    {
        if (!device || sources.size() != 6 ||
            std::any_of(sources.begin(), sources.end(), [](const Texture* t) { return !t || !t->isCubemap(); })) {
            spdlog::error("EnvLighting::generatePrefilteredAtlas: needs six cubemaps");
            return nullptr;
        }
        for (const Texture* source : sources) {
            if (source->encoding() != TextureEncoding::Default) {
                spdlog::warn("EnvLighting::generatePrefilteredAtlas: an RGBM / RGBP source is read "
                    "undecoded; the reproject shader takes linear sources only");
                break;
            }
        }

        TextureOptions atlasOptions;
        atlasOptions.name = "envPrefilteredAtlas";
        atlasOptions.width = size;
        atlasOptions.height = size;
        atlasOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        atlasOptions.mipmaps = false;
        atlasOptions.minFilter = FilterMode::FILTER_LINEAR;
        atlasOptions.magFilter = FilterMode::FILTER_LINEAR;
        auto* texture = new Texture(device, atlasOptions);
        texture->setEncoding(TextureEncoding::RGBP);
        texture->upload();

        // The layout at s = size / 512: the mipmaps of sources[0] down the
        // diagonal, the blurry levels of sources[1..5] down the left of the lower half,
        // and the ambient rect.
        const int s = std::max(1, size / 512);
        EnvAtlasRequest request;
        request.target = texture;
        request.reprojectSource = sources[0];
        request.reprojectSourceProjection = TextureProjection::TEXTUREPROJECTION_CUBE;
        request.encodeRgbp = true;
        request.decodeSrgb = false;
        {
            int x = 0, y = 0, w = 512 * s, h = 256 * s;
            const int levels = calcLevels(512);
            for (int i = 0; i < levels; ++i) {
                request.reprojectRects.push_back({x, y, w, h, s});
                request.reprojectRectSources.push_back(sources[0]);
                x += h;
                y += h;
                w = std::max(1, w / 2);
                h = std::max(1, h / 2);
            }
        }
        {
            int x = 0, y = 256 * s, w = 256 * s, h = 128 * s;
            for (size_t i = 1; i < sources.size(); ++i) {
                request.reprojectRects.push_back({x, y, w, h, s});
                request.reprojectRectSources.push_back(sources[i]);
                y += h;
                w = std::max(1, w / 2);
                h = std::max(1, h / 2);
            }
        }
        const EnvBakeRect ambientRect{128 * s, (256 + 128) * s, 64 * s, 32 * s, s};
        std::vector<float> lambertSamples;
        if (legacyAmbient) {
            request.reprojectRects.push_back(ambientRect);
            request.reprojectRectSources.push_back(sources[5]);
        } else {
            const int sourceSize = static_cast<int>(sources[0]->width());
            lambertSamples = generateLambertSampleTable(numAmbientSamples, sourceSize * sourceSize * 6);
            request.convolveSource = sources[0];
            request.convolveRects.push_back({ambientRect, lambertSamples.data(), numAmbientSamples, false});
        }

        if (!bakeEnvAtlas(device, request)) {
            spdlog::error("EnvLighting::generatePrefilteredAtlas: bake failed");
        }
        return texture;
    }

    // ----------------------------------------------------------------
    // generateSkyboxCubemap
    // ----------------------------------------------------------------
    Texture* EnvLighting::generateSkyboxCubemap(GraphicsDevice* device, Texture* source, int size)
    {
        if (!device || !source) {
            spdlog::warn("EnvLighting::generateSkyboxCubemap: null device or source");
            return nullptr;
        }

        // An RGBA32F equirect (an HDR) is taken as linear; an RGBA8 one (an image) holds
        // gamma-encoded bytes, which the bake decodes after the sampler has filtered them.
        const bool bytes = source->format() == PixelFormat::PIXELFORMAT_RGBA8;
        if (!bytes && source->format() != PixelFormat::PIXELFORMAT_RGBA32F) {
            spdlog::warn("EnvLighting::generateSkyboxCubemap: the source needs RGBA32F or RGBA8 pixel data");
            return nullptr;
        }

        if (size <= 0) {
            size = static_cast<int>(source->width()) / 4;
        }
        // Clamp to what this device will allocate as a cube face, not to a
        // literal: the default size follows the source equirect's width, so a
        // large HDR can ask for more than the hardware accepts.
        size = std::clamp(size, 4, device->maxCubeMapSize());

        TextureOptions options;
        options.name = "skyboxCubemap";
        options.width = static_cast<uint32_t>(size);
        options.height = static_cast<uint32_t>(size);
        options.format = PixelFormat::PIXELFORMAT_RGBA8;
        options.cubemap = true;
        options.mipmaps = false;
        options.minFilter = FilterMode::FILTER_LINEAR;
        options.magFilter = FilterMode::FILTER_LINEAR;

        // RGBP keeps the HDR range in eight bits, as the env atlas does.
        auto* texture = new Texture(device, options);
        texture->setEncoding(TextureEncoding::RGBP);
        texture->upload();

        if (!bakeEquirectToCubemap(device, source, texture, bytes, true)) {
            spdlog::error("EnvLighting::generateSkyboxCubemap: bake failed");
            delete texture;
            return nullptr;
        }
        return texture;
    }
}
