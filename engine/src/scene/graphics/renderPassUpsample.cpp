// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
//
#include "renderPassUpsample.h"

#include "scene/graphics/quadShaderSource.h"
#include "scene/graphics/quadShader.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shader.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr const char* UPSAMPLE_SOURCE = VT_QUAD_MSL_PRELUDE VT_QUAD_MSL_VERTEX(upsampleVertex) R"(
fragment float4 upsampleFragment(
    QuadVarying in [[stage_in]],
    texture2d<float> sourceTexture [[texture(0)]],
    sampler linearSampler [[sampler(0)]])
{
    // 3x3 tent filter upsample (as in LearnOpenGL Phys-Based Bloom).
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

)" VT_QUAD_GLSL_VERTEX R"(
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
        setShader(getOrCreateQuadShader(device.get(), "UpsampleQuad", "upsampleVertex", "upsampleFragment",
            UPSAMPLE_SOURCE, UPSAMPLE_GLSL_SOURCE));
    }

    void RenderPassUpsample::execute()
    {
        setQuadTextureBinding(0, _sourceTexture);
        RenderPassShaderQuad::execute();
    }
}
