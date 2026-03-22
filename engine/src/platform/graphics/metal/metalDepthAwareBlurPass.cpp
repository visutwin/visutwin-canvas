// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Depth-aware bilateral blur pass implementation.
// Shader ported from upstream scene/shader-lib/glsl/chunks/render-pass/frag/depthAwareBlur.js
//
#include "metalDepthAwareBlurPass.h"

#include "metalComposePass.h"
#include "metalGraphicsDevice.h"
#include "metalRenderPipeline.h"
#include "metalTexture.h"
#include "metalVertexBuffer.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    namespace
    {
        // Depth-aware blur (GLSL to Metal).
        // Bilateral blur filter respecting depth discontinuities to avoid halo artifacts.
        // The HORIZONTAL define is prepended at compile time based on the pass direction.
        constexpr const char* BLUR_SOURCE_HORIZONTAL = R"(
#include <metal_stdlib>
using namespace metal;

#define HORIZONTAL 1

struct ComposeVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv0 [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float2 uv1 [[attribute(4)]];
};

struct BlurVarying {
    float4 position [[position]];
    float2 uv;
};

struct BlurUniforms {
    float2 sourceInvResolution;
    int filterSize;
    float cameraNear;
    float cameraFar;
};

vertex BlurVarying blurVertex(ComposeVertexIn in [[stage_in]])
{
    BlurVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

static inline float getLinearDepth(float rawDepth, float cameraNear, float cameraFar)
{
    return (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
}

static inline float bilateralWeight(float depth, float sampleDepth)
{
    float diff = (sampleDepth - depth);
    return max(0.0, 1.0 - diff * diff);
}

static inline void tap(thread float& sum, thread float& totalWeight, float weight, float depth,
    float2 position, texture2d<float> sourceTexture, depth2d<float> depthTexture,
    sampler linearSampler, float cameraNear, float cameraFar)
{
    float color = sourceTexture.sample(linearSampler, position).r;
    float textureDepth = getLinearDepth(depthTexture.sample(linearSampler, position), cameraNear, cameraFar);

    float bilateral = bilateralWeight(depth, textureDepth);
    bilateral *= weight;
    sum += color * bilateral;
    totalWeight += bilateral;
}

fragment float4 blurFragment(
    BlurVarying in [[stage_in]],
    texture2d<float> sourceTexture [[texture(0)]],
    depth2d<float> depthTexture [[texture(1)]],
    sampler linearSampler [[sampler(0)]],
    constant BlurUniforms& uniforms [[buffer(5)]])
{
    const float2 uv = clamp(in.uv, float2(0.0), float2(1.0));

    // handle the center pixel separately because it doesn't participate in bilateral filtering
    float depth = getLinearDepth(depthTexture.sample(linearSampler, uv), uniforms.cameraNear, uniforms.cameraFar);
    float totalWeight = 1.0;
    float color = sourceTexture.sample(linearSampler, uv).r;
    float sum = color * totalWeight;

    // Gaussian sigma: filterSize / 3 gives ~99.7% of the bell within the kernel
    float sigma = max(float(uniforms.filterSize) / 3.0, 1.0);
    float invSigma2 = 1.0 / (2.0 * sigma * sigma);

    for (int i = -uniforms.filterSize; i <= uniforms.filterSize; i++) {
        float weight = exp(-float(i * i) * invSigma2);

        #ifdef HORIZONTAL
            float2 offset = float2(float(i), 0.0) * uniforms.sourceInvResolution;
        #else
            float2 offset = float2(0.0, float(i)) * uniforms.sourceInvResolution;
        #endif

        tap(sum, totalWeight, weight, depth, uv + offset, sourceTexture, depthTexture, linearSampler,
            uniforms.cameraNear, uniforms.cameraFar);
    }

    float ao = sum / totalWeight;
    return float4(ao, 0.0, 0.0, 1.0);
}
)";

        constexpr const char* BLUR_SOURCE_VERTICAL = R"(
#include <metal_stdlib>
using namespace metal;

struct ComposeVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv0 [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float2 uv1 [[attribute(4)]];
};

struct BlurVarying {
    float4 position [[position]];
    float2 uv;
};

struct BlurUniforms {
    float2 sourceInvResolution;
    int filterSize;
    float cameraNear;
    float cameraFar;
};

vertex BlurVarying blurVertex(ComposeVertexIn in [[stage_in]])
{
    BlurVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

static inline float getLinearDepth(float rawDepth, float cameraNear, float cameraFar)
{
    return (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
}

static inline float bilateralWeight(float depth, float sampleDepth)
{
    float diff = (sampleDepth - depth);
    return max(0.0, 1.0 - diff * diff);
}

static inline void tap(thread float& sum, thread float& totalWeight, float weight, float depth,
    float2 position, texture2d<float> sourceTexture, depth2d<float> depthTexture,
    sampler linearSampler, float cameraNear, float cameraFar)
{
    float color = sourceTexture.sample(linearSampler, position).r;
    float textureDepth = getLinearDepth(depthTexture.sample(linearSampler, position), cameraNear, cameraFar);

    float bilateral = bilateralWeight(depth, textureDepth);
    bilateral *= weight;
    sum += color * bilateral;
    totalWeight += bilateral;
}

fragment float4 blurFragment(
    BlurVarying in [[stage_in]],
    texture2d<float> sourceTexture [[texture(0)]],
    depth2d<float> depthTexture [[texture(1)]],
    sampler linearSampler [[sampler(0)]],
    constant BlurUniforms& uniforms [[buffer(5)]])
{
    const float2 uv = clamp(in.uv, float2(0.0), float2(1.0));

    float depth = getLinearDepth(depthTexture.sample(linearSampler, uv), uniforms.cameraNear, uniforms.cameraFar);
    float totalWeight = 1.0;
    float color = sourceTexture.sample(linearSampler, uv).r;
    float sum = color * totalWeight;

    // Gaussian sigma: filterSize / 3 gives ~99.7% of the bell within the kernel
    float sigma = max(float(uniforms.filterSize) / 3.0, 1.0);
    float invSigma2 = 1.0 / (2.0 * sigma * sigma);

    for (int i = -uniforms.filterSize; i <= uniforms.filterSize; i++) {
        float weight = exp(-float(i * i) * invSigma2);

        // Vertical: offset along Y axis
        float2 offset = float2(0.0, float(i)) * uniforms.sourceInvResolution;

        tap(sum, totalWeight, weight, depth, uv + offset, sourceTexture, depthTexture, linearSampler,
            uniforms.cameraNear, uniforms.cameraFar);
    }

    float ao = sum / totalWeight;
    return float4(ao, 0.0, 0.0, 1.0);
}
)";
    }

    MetalDepthAwareBlurPass::MetalDepthAwareBlurPass(MetalGraphicsDevice* device, MetalComposePass* composePass, bool horizontal)
        : _device(device), _composePass(composePass), _horizontal(horizontal)
    {
    }

    MetalDepthAwareBlurPass::~MetalDepthAwareBlurPass()
    {
        if (_depthStencilState) {
            _depthStencilState->release();
            _depthStencilState = nullptr;
        }
    }

    void MetalDepthAwareBlurPass::ensureResources()
    {
        // Ensure the compose pass's shared vertex buffer/format are created first
        _composePass->ensureResources();

        if (_shader && _composePass->vertexBuffer() && _composePass->vertexFormat() &&
            _blendState && _depthState && _depthStencilState) {
            return;
        }

        if (!_shader) {
            ShaderDefinition definition;
            definition.name = _horizontal ? "DepthAwareBlurHorizontalPass" : "DepthAwareBlurVerticalPass";
            definition.vshader = "blurVertex";
            definition.fshader = "blurFragment";
            const char* source = _horizontal ? BLUR_SOURCE_HORIZONTAL : BLUR_SOURCE_VERTICAL;
            _shader = createShader(_device, definition, source);
        }

        if (!_blendState) {
            _blendState = std::make_shared<BlendState>();
        }
        if (!_depthState) {
            _depthState = std::make_shared<DepthState>();
        }
        if (!_depthStencilState && _device->raw()) {
            auto* depthDesc = MTL::DepthStencilDescriptor::alloc()->init();
            depthDesc->setDepthCompareFunction(MTL::CompareFunctionAlways);
            depthDesc->setDepthWriteEnabled(false);
            _depthStencilState = _device->raw()->newDepthStencilState(depthDesc);
            depthDesc->release();
        }
    }

    void MetalDepthAwareBlurPass::execute(MTL::RenderCommandEncoder* encoder,
        const DepthAwareBlurPassParams& params,
        MetalRenderPipeline* pipeline, const std::shared_ptr<RenderTarget>& renderTarget,
        const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats,
        MTL::SamplerState* defaultSampler, MTL::DepthStencilState* defaultDepthStencilState)
    {
        if (!encoder || !params.sourceTexture || !params.depthTexture) {
            return;
        }

        ensureResources();
        if (!_shader || !_composePass->vertexBuffer() || !_composePass->vertexFormat() || !_blendState || !_depthState) {
            spdlog::warn("[executeDepthAwareBlurPass] missing blur resources");
            return;
        }

        Primitive primitive;
        primitive.type = PRIMITIVE_TRIANGLES;
        primitive.base = 0;
        primitive.count = 3;
        primitive.indexed = false;

        auto pipelineState = pipeline->get(primitive, _composePass->vertexFormat(), nullptr, -1, _shader, renderTarget,
            bindGroupFormats, _blendState, _depthState, CullMode::CULLFACE_NONE, false, nullptr, nullptr);
        if (!pipelineState) {
            spdlog::warn("[executeDepthAwareBlurPass] failed to get pipeline state");
            return;
        }

        auto* vb = dynamic_cast<MetalVertexBuffer*>(_composePass->vertexBuffer().get());
        if (!vb || !vb->raw()) {
            spdlog::warn("[executeDepthAwareBlurPass] missing vertex buffer");
            return;
        }

        encoder->setRenderPipelineState(pipelineState);
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setDepthStencilState(_depthStencilState ? _depthStencilState : defaultDepthStencilState);
        encoder->setVertexBuffer(vb->raw(), 0, 0);

        auto* sourceHw = dynamic_cast<gpu::MetalTexture*>(params.sourceTexture->impl());
        auto* depthHw = dynamic_cast<gpu::MetalTexture*>(params.depthTexture->impl());

        encoder->setFragmentTexture(sourceHw ? sourceHw->raw() : nullptr, 0);
        encoder->setFragmentTexture(depthHw ? depthHw->raw() : nullptr, 1);
        if (defaultSampler) {
            encoder->setFragmentSamplerState(defaultSampler, 0);
        }

        struct alignas(16) BlurUniforms
        {
            float sourceInvResolution[2];
            int32_t filterSize;
            float cameraNear;
            float cameraFar;
        } uniforms{};

        uniforms.sourceInvResolution[0] = params.sourceInvResolutionX;
        uniforms.sourceInvResolution[1] = params.sourceInvResolutionY;
        uniforms.filterSize = params.filterSize;
        uniforms.cameraNear = params.cameraNear;
        uniforms.cameraFar = params.cameraFar;
        encoder->setFragmentBytes(&uniforms, sizeof(BlurUniforms), 5);

        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, static_cast<NS::UInteger>(0),
            static_cast<NS::UInteger>(3));
        _device->recordDrawCall();
    }
}
