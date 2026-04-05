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
        // Two variants compiled separately. Matches PlayCanvas's BOXFILTER/!BOXFILTER split:
        //  - SIMPLE (options.boxFilter=true): single bilinear fetch + optional negative clamp.
        //    Used for the scene-half pre-bloom pass where we only want a crisp half-res copy.
        //  - KARIS (options.boxFilter=false, default): 13-tap partial-average filter from the
        //    Call of Duty "Next Generation Post Processing" talk. Used for the bloom mip chain —
        //    its firefly-damping weights and wider support are what produce a smooth HDR halo.
        constexpr const char* DOWNSAMPLE_SOURCE_SIMPLE = R"(
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
    // Single bilinear tap (2x2 averaged by hardware sampler). Used for the scene-half pre-bloom
    // downsample — we want a crisp, non-blurry half-res copy of the scene.
    float3 value = sourceTexture.sample(linearSampler, clamp(in.uv, float2(0.0), float2(1.0))).rgb;
    // Clamp invalid/negative values so bloom & DOF don't propagate NaN/Inf from the scene
    // texture. Matches PlayCanvas's REMOVE_INVALID path.
    value = max(value, float3(0.0));
    return float4(value, 1.0);
}
)";

        constexpr const char* DOWNSAMPLE_SOURCE_KARIS = R"(
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
    // 13-tap Karis partial-average (Call of Duty: Advanced Warfare — Next Generation Post
    // Processing). Same weights PlayCanvas uses for its bloom mip-chain downsample
    // (render-pass/frag/downsample.js). Damps fireflies at each mip level so a single very
    // bright pixel doesn't cascade unfiltered through the chain.
    const float2 texel = float2(1.0 / float(sourceTexture.get_width()),
                                1.0 / float(sourceTexture.get_height()));
    const float2 uv = clamp(in.uv, float2(0.0), float2(1.0));
    const float x = texel.x;
    const float y = texel.y;

    float3 e = sourceTexture.sample(linearSampler, uv).rgb;

    // outer ring (corners + mid-edges) at 2*texel offset
    float3 a = sourceTexture.sample(linearSampler, float2(uv.x - 2.0 * x, uv.y + 2.0 * y)).rgb;
    float3 b = sourceTexture.sample(linearSampler, float2(uv.x,           uv.y + 2.0 * y)).rgb;
    float3 c = sourceTexture.sample(linearSampler, float2(uv.x + 2.0 * x, uv.y + 2.0 * y)).rgb;
    float3 d = sourceTexture.sample(linearSampler, float2(uv.x - 2.0 * x, uv.y              )).rgb;
    float3 f = sourceTexture.sample(linearSampler, float2(uv.x + 2.0 * x, uv.y              )).rgb;
    float3 g = sourceTexture.sample(linearSampler, float2(uv.x - 2.0 * x, uv.y - 2.0 * y)).rgb;
    float3 h = sourceTexture.sample(linearSampler, float2(uv.x,           uv.y - 2.0 * y)).rgb;
    float3 i = sourceTexture.sample(linearSampler, float2(uv.x + 2.0 * x, uv.y - 2.0 * y)).rgb;

    // inner diamond at texel offset (contributes half the total weight)
    float3 j = sourceTexture.sample(linearSampler, float2(uv.x - x, uv.y + y)).rgb;
    float3 k = sourceTexture.sample(linearSampler, float2(uv.x + x, uv.y + y)).rgb;
    float3 l = sourceTexture.sample(linearSampler, float2(uv.x - x, uv.y - y)).rgb;
    float3 m = sourceTexture.sample(linearSampler, float2(uv.x + x, uv.y - y)).rgb;

    float3 value = e * 0.125;
    value += (a + c + g + i) * 0.03125;
    value += (b + d + f + h) * 0.0625;
    value += (j + k + l + m) * 0.125;

    value = max(value, float3(0.0));
    return float4(value, 1.0);
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
        // the AGX compiled-variants footprint limit. Two cache entries because the two
        // filter variants (simple vs Karis) share symbol names but different bodies.
        const char* cacheKey = options.boxFilter ? "DownsampleQuad:Box" : "DownsampleQuad:Karis";
        const char* sourceText = options.boxFilter ? DOWNSAMPLE_SOURCE_SIMPLE : DOWNSAMPLE_SOURCE_KARIS;
        auto cached = device->getCachedShader(cacheKey);
        if (!cached) {
            ShaderDefinition shaderDefinition;
            shaderDefinition.name = cacheKey;
            shaderDefinition.vshader = "downsampleVertex";
            shaderDefinition.fshader = "downsampleFragment";
            cached = createShader(device.get(), shaderDefinition, sourceText);
            device->setCachedShader(cacheKey, cached);
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
