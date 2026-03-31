// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Circle of Confusion (CoC) pass implementation.
// Reads the depth buffer and computes per-pixel CoC values for Depth of Field.
//
#include "metalCoCPass.h"

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
        // CoC (Circle of Confusion) shader — GLSL to Metal.
        // Reads depth buffer and computes CoC for far and optionally near blur.
        constexpr const char* COC_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

struct ComposeVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv0 [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float2 uv1 [[attribute(4)]];
};

struct CoCVarying {
    float4 position [[position]];
    float2 uv;
};

struct CoCUniforms {
    float focusDistance;
    float focusRange;
    float cameraNear;
    float cameraFar;
    float nearBlur;
};

vertex CoCVarying cocVertex(ComposeVertexIn in [[stage_in]])
{
    CoCVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

static inline float getLinearDepth(float rawDepth, float cameraNear, float cameraFar)
{
    return (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
}

fragment float4 cocFragment(
    CoCVarying in [[stage_in]],
    depth2d<float> depthTexture [[texture(0)]],
    sampler linearSampler [[sampler(0)]],
    constant CoCUniforms& uniforms [[buffer(5)]])
{
    float2 uv = clamp(in.uv, float2(0.0), float2(1.0));
    float rawDepth = depthTexture.sample(linearSampler, uv);
    float linearDepth = getLinearDepth(rawDepth, uniforms.cameraNear, uniforms.cameraFar);

    float cocFar = saturate((linearDepth - uniforms.focusDistance) / uniforms.focusRange);

    if (uniforms.nearBlur > 0.5) {
        float cocNear = saturate((uniforms.focusDistance - linearDepth) / uniforms.focusRange);
        return float4(cocFar, cocNear, 0.0, 1.0);
    }
    return float4(cocFar, 0.0, 0.0, 1.0);
}
)";
    }

    MetalCoCPass::MetalCoCPass(MetalGraphicsDevice* device, MetalComposePass* composePass)
        : _device(device), _composePass(composePass)
    {
    }

    MetalCoCPass::~MetalCoCPass()
    {
        if (_depthStencilState) {
            _depthStencilState->release();
            _depthStencilState = nullptr;
        }
    }

    void MetalCoCPass::ensureResources()
    {
        // Ensure the compose pass's shared vertex buffer/format are created first
        _composePass->ensureResources();

        if (_shader && _composePass->vertexBuffer() && _composePass->vertexFormat() &&
            _blendState && _depthState && _depthStencilState) {
            return;
        }

        if (!_shader) {
            ShaderDefinition definition;
            definition.name = "CoCPass";
            definition.vshader = "cocVertex";
            definition.fshader = "cocFragment";
            _shader = createShader(_device, definition, COC_SOURCE);
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

    void MetalCoCPass::execute(MTL::RenderCommandEncoder* encoder,
        const CoCPassParams& params,
        MetalRenderPipeline* pipeline, const std::shared_ptr<RenderTarget>& renderTarget,
        const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats,
        MTL::SamplerState* defaultSampler, MTL::DepthStencilState* defaultDepthStencilState)
    {
        if (!encoder || !params.depthTexture) {
            return;
        }

        ensureResources();
        if (!_shader || !_composePass->vertexBuffer() || !_composePass->vertexFormat() || !_blendState || !_depthState) {
            spdlog::warn("[executeCoCPass] missing CoC resources");
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
            spdlog::warn("[executeCoCPass] failed to get pipeline state");
            return;
        }

        auto* vb = dynamic_cast<MetalVertexBuffer*>(_composePass->vertexBuffer().get());
        if (!vb || !vb->raw()) {
            spdlog::warn("[executeCoCPass] missing vertex buffer");
            return;
        }

        encoder->setRenderPipelineState(pipelineState);
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setDepthStencilState(_depthStencilState ? _depthStencilState : defaultDepthStencilState);
        encoder->setVertexBuffer(vb->raw(), 0, 0);

        auto* depthHw = dynamic_cast<gpu::MetalTexture*>(params.depthTexture->impl());
        encoder->setFragmentTexture(depthHw ? depthHw->raw() : nullptr, 0);
        if (defaultSampler) {
            encoder->setFragmentSamplerState(defaultSampler, 0);
        }

        // CoCUniforms — all scalars, no float2 alignment issues.
        struct alignas(16) CoCUniforms
        {
            float focusDistance;    // offset 0
            float focusRange;      // offset 4
            float cameraNear;      // offset 8
            float cameraFar;       // offset 12
            float nearBlur;        // offset 16
        } uniforms{};

        uniforms.focusDistance = params.focusDistance;
        uniforms.focusRange = params.focusRange;
        uniforms.cameraNear = params.cameraNear;
        uniforms.cameraFar = params.cameraFar;
        uniforms.nearBlur = params.nearBlur ? 1.0f : 0.0f;
        encoder->setFragmentBytes(&uniforms, sizeof(CoCUniforms), 5);

        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, static_cast<NS::UInteger>(0),
            static_cast<NS::UInteger>(3));
        _device->recordDrawCall();
    }
}
