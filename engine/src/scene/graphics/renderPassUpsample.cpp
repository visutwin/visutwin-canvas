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
    return sourceTexture.sample(linearSampler, clamp(in.uv, float2(0.0), float2(1.0)));
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
