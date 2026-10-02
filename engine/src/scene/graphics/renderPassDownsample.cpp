// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
//
#include "renderPassDownsample.h"

#include <string>

#include "scene/graphics/quadShaderSource.h"
#include "scene/graphics/quadShader.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shader.h"

namespace visutwin::canvas
{
    namespace
    {
        // Two variants compiled separately:
        //  - SIMPLE (options.boxFilter=true): single bilinear fetch + optional negative clamp.
        //    Used for the scene-half pre-bloom pass where we only want a crisp half-res copy.
        //  - KARIS (options.boxFilter=false, default): 13-tap partial-average filter from the
        //    Call of Duty "Next Generation Post Processing" talk. Used for the bloom mip chain —
        //    its firefly-damping weights and wider support are what produce a smooth HDR halo.
        constexpr const char* DOWNSAMPLE_SOURCE_SIMPLE = VT_QUAD_MSL_PRELUDE VT_QUAD_MSL_VERTEX(downsampleVertex) R"(
fragment float4 downsampleFragment(
    QuadVarying in [[stage_in]],
    texture2d<float> sourceTexture [[texture(0)]],
    sampler linearSampler [[sampler(0)]])
{
    // Single bilinear tap (2x2 averaged by hardware sampler). Used for the scene-half pre-bloom
    // downsample — we want a crisp, non-blurry half-res copy of the scene.
    float3 value = sourceTexture.sample(linearSampler, clamp(in.uv, float2(0.0), float2(1.0))).rgb;
    // Clamp invalid/negative values so bloom & DOF don't propagate NaN/Inf from the scene
    // texture.
    value = max(value, float3(0.0));
    return float4(value, 1.0);
}
)";

        // Box filter that also multiplies the source by one channel of a second
        // texture at the same uv. The depth-of-field
        // far pass uses it to weight the scene by the far circle of confusion before
        // blurring, so in-focus pixels do not bleed into the blur. `{CH}` is the
        // channel letter, substituted when the variant is built.
        constexpr const char* DOWNSAMPLE_SOURCE_SIMPLE_PREMULTIPLY = VT_QUAD_MSL_PRELUDE VT_QUAD_MSL_VERTEX(downsampleVertex) R"(
fragment float4 downsampleFragment(
    QuadVarying in [[stage_in]],
    texture2d<float> sourceTexture [[texture(0)]],
    texture2d<float> premultiplyTexture [[texture(1)]],
    sampler linearSampler [[sampler(0)]])
{
    const float2 uv = clamp(in.uv, float2(0.0), float2(1.0));
    float3 value = max(sourceTexture.sample(linearSampler, uv).rgb, float3(0.0));
    value *= premultiplyTexture.sample(linearSampler, uv).{CH};
    return float4(value, 1.0);
}
)";

        constexpr const char* DOWNSAMPLE_SOURCE_KARIS = VT_QUAD_MSL_PRELUDE R"(

{PREFILTER_DECL}
)" VT_QUAD_MSL_VERTEX(downsampleVertex) R"(
fragment float4 downsampleFragment(
    QuadVarying in [[stage_in]],
    texture2d<float> sourceTexture [[texture(0)]],
    sampler linearSampler [[sampler(0)]]{PREFILTER_ARG})
{
    // 13-tap Karis partial-average (Call of Duty: Advanced Warfare — Next Generation Post
    // Processing). Damps fireflies at each mip level so a single very
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
{PREFILTER_APPLY}    return float4(value, 1.0);
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

)" VT_QUAD_GLSL_VERTEX R"(
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

        // GLSL twin of DOWNSAMPLE_SOURCE_SIMPLE_PREMULTIPLY.
        constexpr const char* DOWNSAMPLE_GLSL_SIMPLE_PREMULTIPLY = R"(
#version 450

)" VT_QUAD_GLSL_VERTEX R"(
#ifdef VT_FRAGMENT_SHADER
layout(set = 1, binding = 0) uniform sampler2D sourceTexture;
layout(set = 1, binding = 1) uniform sampler2D premultiplyTexture;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;
void main() {
    // Single bilinear tap (2x2 averaged by the hardware sampler), then clamp away
    // negative/invalid values so bloom & DOF cannot propagate NaN/Inf.
    vec3 value = texture(sourceTexture, clamp(vUv, vec2(0.0), vec2(1.0))).rgb;
    value = max(value, vec3(0.0)) * texture(premultiplyTexture, clamp(vUv, vec2(0.0), vec2(1.0))).{CH};
    fragColor = vec4(value, 1.0);
}
#endif
)";

        constexpr const char* DOWNSAMPLE_GLSL_KARIS = R"(
#version 450

)" VT_QUAD_GLSL_VERTEX R"(
#ifdef VT_FRAGMENT_SHADER
{PREFILTER_DECL}layout(set = 1, binding = 0) uniform sampler2D sourceTexture;
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

    value = max(value, vec3(0.0));
{PREFILTER_APPLY}    fragColor = vec4(value, 1.0);
}
#endif
)";

        // A soft-knee high pass on the Karis result, compiled
        // only into the FIRST bloom downsample and only while the bloom threshold is above
        // zero. It scales the filtered value down by how far its brightest channel sits
        // below the threshold, with a quadratic knee (half the threshold)
        // smoothing the transition. It runs on the FILTERED result rather than per tap,
        // so an isolated bright pixel that the filter has already averaged
        // down needs a proportionally lower threshold. The Karis sources carry three
        // markers that are substituted with these, or with nothing for the plain variant.
        constexpr const char* PREFILTER_MSL_DECL = R"(struct PrefilterUniforms {
    float4 thresholdKnee;   // x: threshold, y: knee
};

)";
        constexpr const char* PREFILTER_MSL_ARG = R"(,
    constant PrefilterUniforms& u [[buffer(3)]])";
        constexpr const char* PREFILTER_MSL_APPLY = R"(
    {
        const float luminance = max(value.r, max(value.g, value.b));
        const float threshold = u.thresholdKnee.x;
        const float knee = u.thresholdKnee.y;
        float soft = clamp(luminance - threshold + knee, 0.0, 2.0 * knee);
        soft = soft * soft / (4.0 * knee + 1e-4);
        value *= clamp(max(soft, luminance - threshold) / max(luminance, 1e-4), 0.0, 1.0);
    }
)";
        constexpr const char* PREFILTER_GLSL_DECL = R"(layout(set = 0, binding = 0) uniform PrefilterUniforms {
    vec4 thresholdKnee;   // x: threshold, y: knee
} u;
)";
        constexpr const char* PREFILTER_GLSL_APPLY = R"(
    {
        float luminance = max(value.r, max(value.g, value.b));
        float threshold = u.thresholdKnee.x;
        float knee = u.thresholdKnee.y;
        float soft = clamp(luminance - threshold + knee, 0.0, 2.0 * knee);
        soft = soft * soft / (4.0 * knee + 1e-4);
        value *= clamp(max(soft, luminance - threshold) / max(luminance, 1e-4), 0.0, 1.0);
    }
)";

        struct alignas(16) PrefilterUniforms
        {
            float thresholdKnee[4];
        };
        static_assert(sizeof(PrefilterUniforms) == 16);

        void replaceAll(std::string& text, const std::string& marker, const std::string& value)
        {
            for (size_t at = text.find(marker); at != std::string::npos; at = text.find(marker, at + value.size())) {
                text.replace(at, marker.size(), value);
            }
        }
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
        // Cache the downsample shader at the device level so that bloom passes
        // (which create many RenderPassDownsample instances) don't each compile
        // a separate MTL::Library with the same source.  This avoids hitting
        // the AGX compiled-variants footprint limit. Two cache entries because the two
        // filter variants (simple vs Karis) share symbol names but different bodies.
        // The premultiplied box variant is only meaningful with a box filter, and is keyed on the channel it reads.
        const bool premultiply = options.boxFilter && options.premultiplyTexture != nullptr;
        const std::string channel(1, premultiply ? options.premultiplySrcChannel : 'x');
        const std::string cacheKeyStorage = premultiply
            ? ("DownsampleQuad:BoxPremultiply:" + channel)
            : std::string(options.boxFilter ? "DownsampleQuad:Box"
                : _prefilter ? "DownsampleQuad:KarisPrefilter" : "DownsampleQuad:Karis");
        const bool boxFilter = options.boxFilter;
        const bool prefilter = _prefilter;
        setShader(getOrCreateQuadShader(device.get(), cacheKeyStorage.c_str(), "downsampleVertex", "downsampleFragment",
            [&](const bool glsl) {
                std::string source;
                if (premultiply) {
                    source = glsl ? DOWNSAMPLE_GLSL_SIMPLE_PREMULTIPLY : DOWNSAMPLE_SOURCE_SIMPLE_PREMULTIPLY;
                    const std::string marker = "{CH}";
                    for (size_t at = source.find(marker); at != std::string::npos; at = source.find(marker, at)) {
                        source.replace(at, marker.size(), channel);
                    }
                } else if (boxFilter) {
                    source = glsl ? DOWNSAMPLE_GLSL_SIMPLE : DOWNSAMPLE_SOURCE_SIMPLE;
                } else {
                    source = glsl ? DOWNSAMPLE_GLSL_KARIS : DOWNSAMPLE_SOURCE_KARIS;
                    replaceAll(source, "{PREFILTER_DECL}",
                        prefilter ? (glsl ? PREFILTER_GLSL_DECL : PREFILTER_MSL_DECL) : "");
                    replaceAll(source, "{PREFILTER_ARG}", prefilter && !glsl ? PREFILTER_MSL_ARG : "");
                    replaceAll(source, "{PREFILTER_APPLY}",
                        prefilter ? (glsl ? PREFILTER_GLSL_APPLY : PREFILTER_MSL_APPLY) : "");
                }
                return source;
            }));
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
