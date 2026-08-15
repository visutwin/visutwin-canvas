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
        // Two variants compiled separately. Matches upstream's BOXFILTER/!BOXFILTER split:
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
    // texture. Matches upstream's REMOVE_INVALID path.
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
    // Processing). Same weights upstream uses for its bloom mip-chain downsample
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

        // GLSL ports of the two variants above for the Vulkan backend, which compiles
        // engine-supplied source with shaderc at runtime. Same weights and same clamps —
        // only the binding syntax differs.
        //
        // The quad vertex buffer already holds NDC positions with Metal-oriented UVs, and
        // the Vulkan backend renders through a negative-height viewport (so NDC +Y is up
        // on both backends). uv0 therefore passes straight through with no flip.
        //
        // The source texture arrives via setQuadTextureBinding(0), which the Vulkan draw
        // path binds at set 1 / binding 0 (the slot Metal uses for fragment texture 0).
        constexpr const char* DOWNSAMPLE_GLSL_SIMPLE = R"(
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
    // Single bilinear tap (2x2 averaged by the hardware sampler), then clamp away
    // negative/invalid values so bloom & DOF cannot propagate NaN/Inf.
    vec3 value = texture(sourceTexture, clamp(vUv, vec2(0.0), vec2(1.0))).rgb;
    fragColor = vec4(max(value, vec3(0.0)), 1.0);
}
#endif
)";

        constexpr const char* DOWNSAMPLE_GLSL_KARIS = R"(
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
    // 13-tap Karis partial-average, same weights as the MSL variant above.
    vec2 texel = 1.0 / vec2(textureSize(sourceTexture, 0));
    vec2 uv = clamp(vUv, vec2(0.0), vec2(1.0));
    float x = texel.x;
    float y = texel.y;

    vec3 e = texture(sourceTexture, uv).rgb;

    // outer ring (corners + mid-edges) at 2*texel offset
    vec3 a = texture(sourceTexture, vec2(uv.x - 2.0 * x, uv.y + 2.0 * y)).rgb;
    vec3 b = texture(sourceTexture, vec2(uv.x,           uv.y + 2.0 * y)).rgb;
    vec3 c = texture(sourceTexture, vec2(uv.x + 2.0 * x, uv.y + 2.0 * y)).rgb;
    vec3 d = texture(sourceTexture, vec2(uv.x - 2.0 * x, uv.y             )).rgb;
    vec3 f = texture(sourceTexture, vec2(uv.x + 2.0 * x, uv.y             )).rgb;
    vec3 g = texture(sourceTexture, vec2(uv.x - 2.0 * x, uv.y - 2.0 * y)).rgb;
    vec3 h = texture(sourceTexture, vec2(uv.x,           uv.y - 2.0 * y)).rgb;
    vec3 i = texture(sourceTexture, vec2(uv.x + 2.0 * x, uv.y - 2.0 * y)).rgb;

    // inner diamond at texel offset (contributes half the total weight)
    vec3 j = texture(sourceTexture, vec2(uv.x - x, uv.y + y)).rgb;
    vec3 k = texture(sourceTexture, vec2(uv.x + x, uv.y + y)).rgb;
    vec3 l = texture(sourceTexture, vec2(uv.x - x, uv.y - y)).rgb;
    vec3 m = texture(sourceTexture, vec2(uv.x + x, uv.y - y)).rgb;

    vec3 value = e * 0.125;
    value += (a + c + g + i) * 0.03125;
    value += (b + d + f + h) * 0.0625;
    value += (j + k + l + m) * 0.125;

    fragColor = vec4(max(value, vec3(0.0)), 1.0);
}
#endif
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
        const bool glsl = device->shaderLanguage() == ShaderLanguage::Glsl;
        const char* sourceText = options.boxFilter
            ? (glsl ? DOWNSAMPLE_GLSL_SIMPLE : DOWNSAMPLE_SOURCE_SIMPLE)
            : (glsl ? DOWNSAMPLE_GLSL_KARIS : DOWNSAMPLE_SOURCE_KARIS);
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
