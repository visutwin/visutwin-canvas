// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "renderPassUpsample.h"

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shader.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr const char* UPSAMPLE_SOURCE = R"(
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

vertex QuadVarying upsampleVertex(QuadVertexIn in [[stage_in]])
{
    QuadVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

fragment float4 upsampleFragment(
    QuadVarying in [[stage_in]],
    texture2d<float> sourceTexture [[texture(0)]],
    sampler linearSampler [[sampler(0)]])
{
    // 3x3 tent filter upsample (matches PlayCanvas upsample.js / LearnOpenGL Phys-Based Bloom).
    // Combined with additive blending during upsampling, this spreads each mip's contribution
    // across a wider area and accumulates all mip levels into bloom_rt[0], producing a much
    // smoother and brighter glow than a single bilinear read.
    const float2 texel = float2(1.0 / float(sourceTexture.get_width()),
                                1.0 / float(sourceTexture.get_height()));
    const float2 uv = clamp(in.uv, float2(0.0), float2(1.0));
    const float x = texel.x;
    const float y = texel.y;

    float3 a = sourceTexture.sample(linearSampler, uv + float2(-x,  y)).rgb;
    float3 b = sourceTexture.sample(linearSampler, uv + float2( 0,  y)).rgb;
    float3 c = sourceTexture.sample(linearSampler, uv + float2( x,  y)).rgb;
    float3 d = sourceTexture.sample(linearSampler, uv + float2(-x,  0)).rgb;
    float3 e = sourceTexture.sample(linearSampler, uv                 ).rgb;
    float3 f = sourceTexture.sample(linearSampler, uv + float2( x,  0)).rgb;
    float3 g = sourceTexture.sample(linearSampler, uv + float2(-x, -y)).rgb;
    float3 h = sourceTexture.sample(linearSampler, uv + float2( 0, -y)).rgb;
    float3 i = sourceTexture.sample(linearSampler, uv + float2( x, -y)).rgb;

    float3 value = e * 0.25;
    value += (b + d + f + h) * 0.125;
    value += (a + c + g + i) * 0.0625;
    return float4(value, 1.0);
}
)";
    }

    RenderPassUpsample::RenderPassUpsample(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture)
        : RenderPassShaderQuad(device), _sourceTexture(sourceTexture)
    {
        // Cache the upsample shader at the device level (same rationale as downsample).
        static constexpr const char* CACHE_KEY = "UpsampleQuad";
        auto cached = device->getCachedShader(CACHE_KEY);
        if (!cached) {
            ShaderDefinition shaderDefinition;
            shaderDefinition.name = CACHE_KEY;
            shaderDefinition.vshader = "upsampleVertex";
            shaderDefinition.fshader = "upsampleFragment";
            cached = createShader(device.get(), shaderDefinition, UPSAMPLE_SOURCE);
            device->setCachedShader(CACHE_KEY, cached);
        }
        setShader(cached);
    }

    void RenderPassUpsample::execute()
    {
        setQuadTextureBinding(0, _sourceTexture);
        RenderPassShaderQuad::execute();
    }
}
