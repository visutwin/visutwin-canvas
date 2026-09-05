// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#include "envReproject.h"

#include <spdlog/spdlog.h>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "envBake.h"

namespace visutwin::canvas
{
    void reprojectTexture(GraphicsDevice* device, const EnvReprojectOptions& options)
    {
        if (!device) {
            spdlog::warn("reprojectTexture: device is null");
            return;
        }
        if (!options.source || !options.target) {
            spdlog::warn("reprojectTexture: source or target is null");
            return;
        }

        if (options.rects.empty()) {
            spdlog::warn("reprojectTexture: no rects specified");
            return;
        }

        // Work out both ends' projections. An explicit option wins; otherwise the
        // textures speak for themselves, and an unset projection means EQUIRECT so
        // that callers written before octahedral support keep their old behaviour.
        const auto resolve = [](const TextureProjection requested, const Texture* texture,
                                const bool forceCube) {
            if (requested != TextureProjection::TEXTUREPROJECTION_NONE) {
                return requested;
            }
            if (forceCube) {
                return TextureProjection::TEXTUREPROJECTION_CUBE;
            }
            const auto fromTexture = texture ? texture->projection()
                                             : TextureProjection::TEXTUREPROJECTION_NONE;
            return fromTexture == TextureProjection::TEXTUREPROJECTION_NONE
                ? TextureProjection::TEXTUREPROJECTION_EQUIRECT
                : fromTexture;
        };

        const TextureProjection sourceProjection = resolve(
            options.sourceProjection, options.source.get(),
            options.sourceIsCubemap || options.source->isCubemap());
        TextureProjection targetProjection = resolve(
            options.targetProjection, options.target.get(), false);

        if (targetProjection == TextureProjection::TEXTUREPROJECTION_CUBE) {
            // A cube target is written face by face by the caller, not by one pass.
            spdlog::warn("reprojectTexture: cubemap targets are not supported; "
                         "treating the target as equirect");
            targetProjection = TextureProjection::TEXTUREPROJECTION_EQUIRECT;
        }

        EnvReprojectRequest request;
        request.target = options.target.get();
        request.source = options.source.get();
        request.sourceProjection = sourceProjection;
        request.targetProjection = targetProjection;
        request.encodeRgbp = options.encodeRgbp;
        request.decodeSrgb = options.decodeSrgb;
        for (const auto& rect : options.rects) {
            request.rects.push_back({rect.rectX, rect.rectY, rect.rectW, rect.rectH, rect.seamPixels});
        }
        bakeReproject(device, request);
    }

    void convolveTexture(GraphicsDevice* device, const EnvConvolveOptions& options)
    {
        if (!device) {
            spdlog::warn("convolveTexture: device is null");
            return;
        }
        if (!options.source || !options.target) {
            spdlog::warn("convolveTexture: source or target is null");
            return;
        }
        if (options.rects.empty()) {
            spdlog::warn("convolveTexture: no rects specified");
            return;
        }

        EnvConvolveRequest request;
        request.target = options.target.get();
        request.source = options.source.get();
        request.encodeRgbp = options.encodeRgbp;
        request.decodeSrgb = options.decodeSrgb;
        request.rects.reserve(options.rects.size());
        for (const auto& r : options.rects) {
            EnvConvolveBakeRect entry;
            entry.rect = {r.rectX, r.rectY,
                r.rectW > 0 ? r.rectW : static_cast<int>(options.target->width()),
                r.rectH > 0 ? r.rectH : static_cast<int>(options.target->height()),
                r.seamPixels};
            entry.samples = r.samples;
            entry.numSamples = r.numSamples;
            entry.weightByNoL = r.weightByNoL;
            request.rects.push_back(entry);
        }
        bakeConvolve(device, request);
    }

    void bakeEnvAtlas(GraphicsDevice* device, const EnvAtlasBakeOptions& options)
    {
        if (!device) {
            spdlog::warn("bakeEnvAtlas: device is null");
            return;
        }
        if (!options.target) {
            spdlog::warn("bakeEnvAtlas: target is null");
            return;
        }

        EnvAtlasRequest request;
        request.target = options.target.get();
        request.encodeRgbp = options.encodeRgbp;
        request.decodeSrgb = options.decodeSrgb;

        // A zero width or height means "the whole target", the convention the
        // callers already used.
        const auto sized = [&](const auto& r) {
            return EnvBakeRect{r.rectX, r.rectY,
                r.rectW > 0 ? r.rectW : static_cast<int>(options.target->width()),
                r.rectH > 0 ? r.rectH : static_cast<int>(options.target->height()),
                r.seamPixels};
        };

        if (options.reprojectSource && !options.reprojectRects.empty()) {
            request.reprojectSource = options.reprojectSource.get();
            request.reprojectSourceProjection = options.reprojectSourceIsCubemap
                ? TextureProjection::TEXTUREPROJECTION_CUBE
                : TextureProjection::TEXTUREPROJECTION_EQUIRECT;
            // The atlas layout is authored in equirect rects whatever the source is.
            request.reprojectTargetProjection = TextureProjection::TEXTUREPROJECTION_EQUIRECT;
            for (const auto& r : options.reprojectRects) {
                request.reprojectRects.push_back(sized(r));
            }
        }

        if (options.convolveSource && !options.convolveRects.empty()) {
            request.convolveSource = options.convolveSource.get();
            for (const auto& r : options.convolveRects) {
                EnvConvolveBakeRect entry;
                entry.rect = sized(r);
                entry.samples = r.samples;
                entry.numSamples = r.numSamples;
                entry.weightByNoL = r.weightByNoL;
                request.convolveRects.push_back(entry);
            }
        }

        bakeEnvAtlas(device, request);
    }

    std::shared_ptr<Texture> equirectToCubemap(GraphicsDevice* device,
        const std::shared_ptr<Texture>& source, int faceSize, bool decodeSrgb)
    {
        if (!device || !source || faceSize <= 0) {
            spdlog::warn("equirectToCubemap: invalid arguments");
            return nullptr;
        }

        TextureOptions options;
        options.name = "envCubemapHdr";
        options.width = static_cast<uint32_t>(faceSize);
        options.height = static_cast<uint32_t>(faceSize);
        options.format = PixelFormat::PIXELFORMAT_RGBA32F;
        options.cubemap = true;
        options.mipmaps = true;
        options.minFilter = FilterMode::FILTER_LINEAR_MIPMAP_LINEAR;
        options.magFilter = FilterMode::FILTER_LINEAR;

        auto target = std::make_shared<Texture>(device, options);
        target->upload();

        if (!bakeEquirectToCubemap(device, source.get(), target.get(), decodeSrgb)) {
            return nullptr;
        }
        return target;
    }
}
