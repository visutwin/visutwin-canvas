// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#include "envReproject.h"

#include <spdlog/spdlog.h>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"

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

        EnvReprojectPassParams params;
        params.target = options.target.get();
        if (sourceProjection == TextureProjection::TEXTUREPROJECTION_CUBE) {
            params.sourceCubemap = options.source.get();
        } else {
            params.sourceEquirect = options.source.get();
        }
        params.sourceProjection = sourceProjection;
        params.targetProjection = targetProjection;

        params.ops.reserve(options.rects.size());
        for (const auto& r : options.rects) {
            EnvReprojectOp op;
            op.rectX = r.rectX;
            op.rectY = r.rectY;
            op.rectW = r.rectW > 0 ? r.rectW : static_cast<int>(options.target->width());
            op.rectH = r.rectH > 0 ? r.rectH : static_cast<int>(options.target->height());
            op.seamPixels = r.seamPixels;
            params.ops.push_back(op);
        }
        params.encodeRgbp = options.encodeRgbp;
        params.decodeSrgb = options.decodeSrgb;

        device->generateEnvReproject(params);
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

        EnvConvolvePassParams params;
        params.target = options.target.get();
        if (options.sourceIsCubemap) {
            params.sourceCubemap = options.source.get();
        } else {
            params.sourceEquirect = options.source.get();
        }

        params.ops.reserve(options.rects.size());
        for (const auto& r : options.rects) {
            EnvConvolveOp op;
            op.rectX = r.rectX;
            op.rectY = r.rectY;
            op.rectW = r.rectW > 0 ? r.rectW : static_cast<int>(options.target->width());
            op.rectH = r.rectH > 0 ? r.rectH : static_cast<int>(options.target->height());
            op.seamPixels = r.seamPixels;
            op.samples = r.samples;
            op.numSamples = r.numSamples;
            op.weightByNoL = r.weightByNoL;
            params.ops.push_back(op);
        }
        params.encodeRgbp = options.encodeRgbp;
        params.decodeSrgb = options.decodeSrgb;

        device->generateEnvConvolve(params);
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

        EnvAtlasBakeParams params;
        params.target = options.target.get();
        params.encodeRgbp = options.encodeRgbp;
        params.decodeSrgb = options.decodeSrgb;

        if (options.reprojectSource && !options.reprojectRects.empty()) {
            if (options.reprojectSourceIsCubemap) {
                params.reprojectSourceCubemap = options.reprojectSource.get();
                params.reprojectSourceProjection = TextureProjection::TEXTUREPROJECTION_CUBE;
            } else {
                params.reprojectSourceEquirect = options.reprojectSource.get();
                params.reprojectSourceProjection = TextureProjection::TEXTUREPROJECTION_EQUIRECT;
            }
            // The atlas layout is authored in equirect rects regardless of the source.
            params.reprojectTargetProjection = TextureProjection::TEXTUREPROJECTION_EQUIRECT;
            params.reprojectOps.reserve(options.reprojectRects.size());
            for (const auto& r : options.reprojectRects) {
                EnvReprojectOp op;
                op.rectX = r.rectX;
                op.rectY = r.rectY;
                op.rectW = r.rectW > 0 ? r.rectW : static_cast<int>(options.target->width());
                op.rectH = r.rectH > 0 ? r.rectH : static_cast<int>(options.target->height());
                op.seamPixels = r.seamPixels;
                params.reprojectOps.push_back(op);
            }
        }

        if (options.convolveSource && !options.convolveRects.empty()) {
            if (options.convolveSourceIsCubemap) {
                params.convolveSourceCubemap = options.convolveSource.get();
            } else {
                params.convolveSourceEquirect = options.convolveSource.get();
            }
            params.convolveOps.reserve(options.convolveRects.size());
            for (const auto& r : options.convolveRects) {
                EnvConvolveOp op;
                op.rectX = r.rectX;
                op.rectY = r.rectY;
                op.rectW = r.rectW > 0 ? r.rectW : static_cast<int>(options.target->width());
                op.rectH = r.rectH > 0 ? r.rectH : static_cast<int>(options.target->height());
                op.seamPixels = r.seamPixels;
                op.samples = r.samples;
                op.numSamples = r.numSamples;
                op.weightByNoL = r.weightByNoL;
                params.convolveOps.push_back(op);
            }
        }

        device->generateEnvAtlas(params);
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

        EquirectToCubeParams params;
        params.source = source.get();
        params.target = target.get();
        params.decodeSrgb = decodeSrgb;
        device->generateEquirectToCubemap(params);

        return target;
    }
}
