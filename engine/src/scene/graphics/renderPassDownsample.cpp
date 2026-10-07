// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
//
#include "renderPassDownsample.h"

#include <string>

#include "scene/shader-lib/slangShaders.h"
#include "spdlog/spdlog.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shader.h"

namespace visutwin::canvas
{
    namespace
    {
        struct alignas(16) PrefilterUniforms
        {
            float thresholdKnee[4];
        };
        static_assert(sizeof(PrefilterUniforms) == 16);
    }

    RenderPassDownsample::RenderPassDownsample(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture)
        : RenderPassDownsample(device, sourceTexture, Options{})
    {
    }

    RenderPassDownsample::RenderPassDownsample(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture,
        const Options& options)
        : RenderPassShaderQuad(device), _sourceTexture(sourceTexture), _premultiplyTexture(options.premultiplyTexture),
          _options(options), _prefilter(options.prefilter && !options.boxFilter)
    {
        // engine/shaders/slang/programs/downsample.slang, one variant per filter, cached at
        // the device level so the many bloom passes share one compiled program each. The
        // premultiplied box variant is only meaningful with a box filter, and is built for
        // the one channel the engine uses (the depth-of-field far pass reads r).
        const bool premultiply = options.boxFilter && options.premultiplyTexture != nullptr;
        if (premultiply && options.premultiplySrcChannel != 'r') {
            spdlog::error("RenderPassDownsample: no premultiply variant for channel '{}' (only 'r' is built)",
                options.premultiplySrcChannel);
        }
        const char* variant = premultiply ? "box-premultiply-r"
            : options.boxFilter ? "box" : _prefilter ? "karis-prefilter" : "karis";
        setShader(getOrCreateSlangShader(device.get(), "downsample", variant));
    }

    void RenderPassDownsample::setSourceTexture(Texture* value)
    {
        _sourceTexture = value;
    }

    void RenderPassDownsample::execute()
    {
        setQuadTextureBinding(0, _sourceTexture);
        setQuadTextureBinding(1, _premultiplyTexture);
        if (_sourceTexture) {
            _sourceInvResolution[0] = _sourceTexture->width() > 0 ? 1.0f / static_cast<float>(_sourceTexture->width()) : 1.0f;
            _sourceInvResolution[1] = _sourceTexture->height() > 0 ? 1.0f / static_cast<float>(_sourceTexture->height()) : 1.0f;
        } else {
            _sourceInvResolution[0] = 1.0f;
            _sourceInvResolution[1] = 1.0f;
        }

        if (_prefilter) {
            PrefilterUniforms uniforms{};
            uniforms.thresholdKnee[0] = _prefilterThreshold;
            uniforms.thresholdKnee[1] = _prefilterKnee;
            setQuadUniforms(uniforms);
        }

        RenderPassShaderQuad::execute();
    }
}
