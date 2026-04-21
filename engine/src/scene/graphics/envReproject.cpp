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

        EnvReprojectPassParams params;
        params.target = options.target.get();
        if (options.sourceIsCubemap) {
            params.sourceCubemap = options.source.get();
        } else {
            params.sourceEquirect = options.source.get();
        }

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
}
