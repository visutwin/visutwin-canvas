// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Separable 1D gaussian blur for EVSM moments. Mirrors upstream blurVSM.js
// (GAUSS path). Operates on the RGB channels of an RGBA16F source; the alpha
// "rendered" flag is preserved by re-emitting 1.0.
//
// This effect lives ABOVE GraphicsDevice: it is a shader, one input texture and
// one uniform block driven through QuadRender, so there is a single
// implementation instead of one per backend. Direction is a uniform rather than
// two compiled variants — the same choice the debug passes make.
//
#include "renderPassVsmBlur.h"

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/texture.h"
#include "scene/graphics/quadRender.h"

namespace visutwin::canvas
{
    namespace
    {
        // Uniform block shared by both languages. Two vec4s keeps std140 and MSL
        // layouts identical without padding rules coming into it.
        struct VsmBlurUniforms
        {
            float invResolutionAndDirection[4];  // xy = 1/resolution, zw = blur direction
            float params[4];                     // x = filterSize, y = cascade tile size
        };

        constexpr const char* VSM_BLUR_MSL = R"(
#include <metal_stdlib>
using namespace metal;

#define MAX_TAPS 25

struct VsmVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv0 [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float2 uv1 [[attribute(4)]];
};

struct VsmVarying {
    float4 position [[position]];
    float2 uv;
};

struct VsmBlurUniforms {
    float4 invResolutionAndDirection;
    float4 params;
};

vertex VsmVarying vsmBlurVertex(VsmVertexIn in [[stage_in]])
{
    VsmVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

fragment float4 vsmBlurFragment(
    VsmVarying in [[stage_in]],
    texture2d<float> sourceTexture [[texture(0)]],
    sampler linearSampler [[sampler(0)]],
    constant VsmBlurUniforms& uniforms [[buffer(3)]])
{
    const float2 invResolution = uniforms.invResolutionAndDirection.xy;
    const float2 direction = uniforms.invResolutionAndDirection.zw;
    const int filterSize = clamp(int(uniforms.params.x), 1, MAX_TAPS / 2);
    const float sigma = max(float(filterSize) / 3.0, 1.0);
    const float invSigma2 = 1.0 / (2.0 * sigma * sigma);

    // Clamp taps to this fragment's cascade tile so the kernel can't mix
    // moments across cascade seams in multi-cascade (quadrant) atlases.
    const float tile = clamp(uniforms.params.y <= 0.0 ? 1.0 : uniforms.params.y, 0.0, 1.0);
    const float2 halfTexel = 0.5 * invResolution;
    const float2 tileMin = (tile < 1.0) ? floor(in.uv / tile) * tile : float2(0.0);
    const float2 clampMin = tileMin + halfTexel;
    const float2 clampMax = tileMin + tile - halfTexel;

    float3 moments = float3(0.0);
    float totalWeight = 0.0;
    for (int i = -filterSize; i <= filterSize; ++i) {
        const float w = exp(-float(i * i) * invSigma2);
        const float2 offset = direction * float(i) * invResolution;
        moments += sourceTexture.sample(linearSampler, clamp(in.uv + offset, clampMin, clampMax)).xyz * w;
        totalWeight += w;
    }
    moments /= max(totalWeight, 1e-6);
    // Preserve the "rendered" flag so the second pass / forward sampling
    // doesn't think every blurred pixel is a cleared synthetic-lit fallback.
    return float4(moments, 1.0);
}
)";

        constexpr const char* VSM_BLUR_GLSL = R"(
#version 450

#define MAX_TAPS 25

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
layout(set = 0, binding = 0) uniform VsmBlurUniforms {
    vec4 invResolutionAndDirection;
    vec4 params;
} uniforms;
layout(set = 1, binding = 0) uniform sampler2D sourceTexture;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

void main() {
    vec2 invResolution = uniforms.invResolutionAndDirection.xy;
    vec2 direction = uniforms.invResolutionAndDirection.zw;
    int filterSize = clamp(int(uniforms.params.x), 1, MAX_TAPS / 2);
    float sigma = max(float(filterSize) / 3.0, 1.0);
    float invSigma2 = 1.0 / (2.0 * sigma * sigma);

    float tile = clamp(uniforms.params.y <= 0.0 ? 1.0 : uniforms.params.y, 0.0, 1.0);
    vec2 halfTexel = 0.5 * invResolution;
    vec2 tileMin = (tile < 1.0) ? floor(vUv / tile) * tile : vec2(0.0);
    vec2 clampMin = tileMin + halfTexel;
    vec2 clampMax = tileMin + tile - halfTexel;

    vec3 moments = vec3(0.0);
    float totalWeight = 0.0;
    for (int i = -filterSize; i <= filterSize; ++i) {
        float w = exp(-float(i * i) * invSigma2);
        vec2 offset = direction * float(i) * invResolution;
        moments += texture(sourceTexture, clamp(vUv + offset, clampMin, clampMax)).xyz * w;
        totalWeight += w;
    }
    moments /= max(totalWeight, 1e-6);
    fragColor = vec4(moments, 1.0);
}
#endif
)";

        std::shared_ptr<Shader> vsmBlurShader(GraphicsDevice* device)
        {
            constexpr const char* cacheKey = "vsm-blur-quad";
            if (auto cached = device->getCachedShader(cacheKey)) {
                return cached;
            }
            ShaderDefinition definition;
            definition.name = cacheKey;
            definition.vshader = "vsmBlurVertex";
            definition.fshader = "vsmBlurFragment";
            const char* source = device->shaderLanguage() == ShaderLanguage::Glsl
                ? VSM_BLUR_GLSL : VSM_BLUR_MSL;
            auto shader = createShader(device, definition, source);
            if (shader) {
                device->setCachedShader(cacheKey, shader);
            }
            return shader;
        }
    }

    RenderPassVsmBlur::RenderPassVsmBlur(const std::shared_ptr<GraphicsDevice>& device,
        Texture* sourceTexture,
        const std::shared_ptr<RenderTarget>& targetRenderTarget,
        const int shadowResolution,
        const bool horizontal,
        const int filterSize,
        const float cascadeTileSize)
        : RenderPass(device),
          _sourceTexture(sourceTexture),
          _shadowResolution(shadowResolution),
          _horizontal(horizontal),
          _filterSize(filterSize),
          _cascadeTileSize(cascadeTileSize)
    {
        _requiresCubemaps = false;
        _name = horizontal ? "RenderPassVsmBlurH" : "RenderPassVsmBlurV";

        init(targetRenderTarget);
        // Full overwrite — no clear required since we touch every pixel of the rect.
        if (colorOps()) {
            colorOps()->clear = false;
        }
        if (depthStencilOps()) {
            depthStencilOps()->clearDepth = false;
            depthStencilOps()->storeDepth = false;
        }
    }

    void RenderPassVsmBlur::execute()
    {
        auto dev = device();
        if (!dev || !_sourceTexture || _shadowResolution <= 0) {
            return;
        }

        auto shader = vsmBlurShader(dev.get());
        if (!shader) {
            return;
        }

        const float invResolution = 1.0f / static_cast<float>(_shadowResolution);
        VsmBlurUniforms uniforms{};
        uniforms.invResolutionAndDirection[0] = invResolution;
        uniforms.invResolutionAndDirection[1] = invResolution;
        uniforms.invResolutionAndDirection[2] = _horizontal ? 1.0f : 0.0f;
        uniforms.invResolutionAndDirection[3] = _horizontal ? 0.0f : 1.0f;
        uniforms.params[0] = static_cast<float>(_filterSize);
        uniforms.params[1] = _cascadeTileSize;

        QuadRender quad(shader);
        quad.setTexture(0, _sourceTexture);
        quad.setUniforms(uniforms);
        quad.render();
    }
}
