// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "renderPassDownsample.h"

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shader.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr const char* DOWNSAMPLE_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

struct QuadVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv0 [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float2 uv1 [[attribute(4)]];
};

struct QuadVarying {
    float4 position [[position]];
    float2 uv;
};

vertex QuadVarying downsampleVertex(QuadVertexIn in [[stage_in]])
{
    QuadVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

fragment float4 downsampleFragment(
    QuadVarying in [[stage_in]],
    texture2d<float> sourceTexture [[texture(0)]],
    sampler linearSampler [[sampler(0)]])
{
    return sourceTexture.sample(linearSampler, clamp(in.uv, float2(0.0), float2(1.0)));
}
)";
    }

    RenderPassDownsample::RenderPassDownsample(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture)
        : RenderPassDownsample(device, sourceTexture, Options{})
    {
    }

    RenderPassDownsample::RenderPassDownsample(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture,
        const Options& options)
        : RenderPassShaderQuad(device), _sourceTexture(sourceTexture), _premultiplyTexture(options.premultiplyTexture), _options(options)
    {
        // Cache the downsample shader at the device level so that bloom passes
        // (which create many RenderPassDownsample instances) don't each compile
        // a separate MTL::Library with the same source.  This avoids hitting
        // the AGX compiled-variants footprint limit.
        static constexpr const char* CACHE_KEY = "DownsampleQuad";
        auto cached = device->getCachedShader(CACHE_KEY);
        if (!cached) {
            ShaderDefinition shaderDefinition;
            shaderDefinition.name = CACHE_KEY;
            shaderDefinition.vshader = "downsampleVertex";
            shaderDefinition.fshader = "downsampleFragment";
            cached = createShader(device.get(), shaderDefinition, DOWNSAMPLE_SOURCE);
            device->setCachedShader(CACHE_KEY, cached);
        }
        setShader(cached);
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

        RenderPassShaderQuad::execute();
    }
}
