// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Separable 1D gaussian blur for EVSM moments. Mirrors upstream blurVSM.js
// (GAUSS path). Operates on the RGB channels of an RGBA16F source; the alpha
// "rendered" flag is preserved by re-emitting 1.0.
//
#include "metalVsmBlurPass.h"

#include "metalComposePass.h"
#include "metalGraphicsDevice.h"
#include "metalRenderPipeline.h"
#include "metalTexture.h"
#include "metalVertexBuffer.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
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
        // Single-direction 11-tap gaussian (filterSize = 5). Direction is
        // selected at compile-time via the HORIZONTAL macro so each variant
        // produces an unambiguous, branchless inner loop.
        constexpr const char* VSM_BLUR_SOURCE_HORIZONTAL = R"(
#include <metal_stdlib>
using namespace metal;

#define HORIZONTAL 1
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
    float2 sourceInvResolution;
    int filterSize;
    int _pad;
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
    constant VsmBlurUniforms& uniforms [[buffer(5)]])
{
    const int filterSize = clamp(uniforms.filterSize, 1, MAX_TAPS / 2);
    const float sigma = max(float(filterSize) / 3.0, 1.0);
    const float invSigma2 = 1.0 / (2.0 * sigma * sigma);

    float3 moments = float3(0.0);
    float totalWeight = 0.0;
    for (int i = -filterSize; i <= filterSize; ++i) {
        const float w = exp(-float(i * i) * invSigma2);
#ifdef HORIZONTAL
        const float2 offset = float2(float(i), 0.0) * uniforms.sourceInvResolution;
#else
        const float2 offset = float2(0.0, float(i)) * uniforms.sourceInvResolution;
#endif
        moments += sourceTexture.sample(linearSampler, in.uv + offset).xyz * w;
        totalWeight += w;
    }
    moments /= max(totalWeight, 1e-6);
    // Preserve the "rendered" flag so the second pass / forward sampling
    // doesn't think every blurred pixel is a cleared synthetic-lit fallback.
    return float4(moments, 1.0);
}
)";

        constexpr const char* VSM_BLUR_SOURCE_VERTICAL = R"(
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
    float2 sourceInvResolution;
    int filterSize;
    int _pad;
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
    constant VsmBlurUniforms& uniforms [[buffer(5)]])
{
    const int filterSize = clamp(uniforms.filterSize, 1, MAX_TAPS / 2);
    const float sigma = max(float(filterSize) / 3.0, 1.0);
    const float invSigma2 = 1.0 / (2.0 * sigma * sigma);

    float3 moments = float3(0.0);
    float totalWeight = 0.0;
    for (int i = -filterSize; i <= filterSize; ++i) {
        const float w = exp(-float(i * i) * invSigma2);
        const float2 offset = float2(0.0, float(i)) * uniforms.sourceInvResolution;
        moments += sourceTexture.sample(linearSampler, in.uv + offset).xyz * w;
        totalWeight += w;
    }
    moments /= max(totalWeight, 1e-6);
    return float4(moments, 1.0);
}
)";
    }

    MetalVsmBlurPass::MetalVsmBlurPass(MetalGraphicsDevice* device, MetalComposePass* composePass, bool horizontal)
        : _device(device), _composePass(composePass), _horizontal(horizontal)
    {
    }

    MetalVsmBlurPass::~MetalVsmBlurPass()
    {
        if (_depthStencilState) {
            _depthStencilState->release();
            _depthStencilState = nullptr;
        }
        if (_linearSampler) {
            _linearSampler->release();
            _linearSampler = nullptr;
        }
    }

    void MetalVsmBlurPass::ensureResources()
    {
        _composePass->ensureResources();

        if (_shader && _composePass->vertexBuffer() && _composePass->vertexFormat() &&
            _blendState && _depthState && _depthStencilState && _linearSampler) {
            return;
        }

        if (!_shader) {
            ShaderDefinition definition;
            definition.name = _horizontal ? "VsmBlurHorizontal" : "VsmBlurVertical";
            definition.vshader = "vsmBlurVertex";
            definition.fshader = "vsmBlurFragment";
            const char* source = _horizontal ? VSM_BLUR_SOURCE_HORIZONTAL : VSM_BLUR_SOURCE_VERTICAL;
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
        if (!_linearSampler && _device->raw()) {
            auto* samplerDesc = MTL::SamplerDescriptor::alloc()->init();
            samplerDesc->setMinFilter(MTL::SamplerMinMagFilterLinear);
            samplerDesc->setMagFilter(MTL::SamplerMinMagFilterLinear);
            samplerDesc->setMipFilter(MTL::SamplerMipFilterNotMipmapped);
            samplerDesc->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
            samplerDesc->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
            _linearSampler = _device->raw()->newSamplerState(samplerDesc);
            samplerDesc->release();
        }
    }

    void MetalVsmBlurPass::execute(MTL::RenderCommandEncoder* encoder,
        const VsmBlurPassParams& params,
        MetalRenderPipeline* pipeline, const std::shared_ptr<RenderTarget>& renderTarget,
        const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats,
        MTL::DepthStencilState* defaultDepthStencilState)
    {
        if (!encoder || !params.sourceTexture) {
            return;
        }

        ensureResources();
        if (!_shader || !_composePass->vertexBuffer() || !_composePass->vertexFormat() ||
            !_blendState || !_depthState) {
            spdlog::warn("[MetalVsmBlurPass] missing resources");
            return;
        }

        Primitive primitive;
        primitive.type = PRIMITIVE_TRIANGLES;
        primitive.base = 0;
        primitive.count = 3;
        primitive.indexed = false;

        auto* pipelineState = pipeline->get(primitive, _composePass->vertexFormat(), nullptr, -1, _shader,
            renderTarget, bindGroupFormats, _blendState, _depthState, CullMode::CULLFACE_NONE,
            false, nullptr, nullptr);
        if (!pipelineState) {
            spdlog::warn("[MetalVsmBlurPass] failed to get pipeline state");
            return;
        }

        auto* vb = dynamic_cast<MetalVertexBuffer*>(_composePass->vertexBuffer().get());
        if (!vb || !vb->raw()) {
            spdlog::warn("[MetalVsmBlurPass] missing vertex buffer");
            return;
        }

        encoder->setRenderPipelineState(pipelineState);
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setDepthStencilState(_depthStencilState ? _depthStencilState : defaultDepthStencilState);
        encoder->setVertexBuffer(vb->raw(), 0, 0);

        auto* sourceHw = dynamic_cast<gpu::MetalTexture*>(params.sourceTexture->impl());
        encoder->setFragmentTexture(sourceHw ? sourceHw->raw() : nullptr, 0);
        if (_linearSampler) {
            encoder->setFragmentSamplerState(_linearSampler, 0);
        }

        struct alignas(16) VsmBlurUniforms
        {
            float sourceInvResolution[2];
            int32_t filterSize;
            int32_t _pad;
        } uniforms{};
        uniforms.sourceInvResolution[0] = params.sourceInvResolutionX;
        uniforms.sourceInvResolution[1] = params.sourceInvResolutionY;
        uniforms.filterSize = params.filterSize;
        uniforms._pad = 0;
        encoder->setFragmentBytes(&uniforms, sizeof(VsmBlurUniforms), 5);

        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
            static_cast<NS::UInteger>(0), static_cast<NS::UInteger>(3));
        _device->recordDrawCall();
    }
}
