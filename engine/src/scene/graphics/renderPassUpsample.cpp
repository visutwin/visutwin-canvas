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

        // GLSL port for the Vulkan backend — same tent weights. The quad source arrives
        // via setQuadTextureBinding(0), bound at set 1 / binding 0; UVs need no flip
        // because the Vulkan backend renders through a negative-height viewport.
        constexpr const char* UPSAMPLE_GLSL_SOURCE = R"(
#version 450

#ifdef VT_VERTEX_SHADER
layout(location = 0) in vec3 vertexPosition;
layout(location = 2) in vec2 vertexUv0;
layout(location = 0) out vec2 vUv;
void main() {
    vUv = vertexUv0;
    gl_Position = vec4(vertexPosition, 1.0);
}
#endif

#ifdef VT_FRAGMENT_SHADER
layout(set = 1, binding = 0) uniform sampler2D sourceTexture;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;
void main() {
    // 3x3 tent filter upsample, same weights as the MSL variant above.
    vec2 texel = 1.0 / vec2(textureSize(sourceTexture, 0));
    vec2 uv = clamp(vUv, vec2(0.0), vec2(1.0));
    float x = texel.x;
    float y = texel.y;

    vec3 a = texture(sourceTexture, uv + vec2(-x,  y)).rgb;
    vec3 b = texture(sourceTexture, uv + vec2(0.0, y)).rgb;
    vec3 c = texture(sourceTexture, uv + vec2( x,  y)).rgb;
    vec3 d = texture(sourceTexture, uv + vec2(-x, 0.0)).rgb;
    vec3 e = texture(sourceTexture, uv                ).rgb;
    vec3 f = texture(sourceTexture, uv + vec2( x, 0.0)).rgb;
    vec3 g = texture(sourceTexture, uv + vec2(-x, -y)).rgb;
    vec3 h = texture(sourceTexture, uv + vec2(0.0, -y)).rgb;
    vec3 i = texture(sourceTexture, uv + vec2( x, -y)).rgb;

    vec3 value = e * 0.25;
    value += (b + d + f + h) * 0.125;
    value += (a + c + g + i) * 0.0625;
    fragColor = vec4(value, 1.0);
}
#endif
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
            cached = createShader(device.get(), shaderDefinition,
                device->shaderLanguage() == ShaderLanguage::Glsl
                    ? UPSAMPLE_GLSL_SOURCE
                    : UPSAMPLE_SOURCE);
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
