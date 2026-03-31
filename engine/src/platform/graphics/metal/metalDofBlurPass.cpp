// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// DOF (Depth of Field) blur pass implementation.
// Samples scene in concentric rings weighted by Circle of Confusion.
//
#include "metalDofBlurPass.h"

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
        // DOF Blur shader — disc blur / bokeh approximation.
        // Samples far texture in concentric rings, weighted by CoC value.
        constexpr const char* DOF_BLUR_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

struct ComposeVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv0 [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float2 uv1 [[attribute(4)]];
};

struct DofBlurVarying {
    float4 position [[position]];
    float2 uv;
};

struct DofBlurUniforms {
    float blurRadiusNear;       // offset 0
    float blurRadiusFar;        // offset 4
    float2 invResolution;       // offset 8  (Metal float2: 8-byte aligned)
    int blurRings;              // offset 16
    int blurRingPoints;         // offset 20
};

vertex DofBlurVarying dofBlurVertex(ComposeVertexIn in [[stage_in]])
{
    DofBlurVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

fragment float4 dofBlurFragment(
    DofBlurVarying in [[stage_in]],
    texture2d<float> farTexture [[texture(0)]],
    texture2d<float> cocTexture [[texture(1)]],
    texture2d<float> nearTexture [[texture(2)]],
    sampler linearSampler [[sampler(0)]],
    constant DofBlurUniforms& uniforms [[buffer(5)]])
{
    float2 uv = clamp(in.uv, float2(0.0), float2(1.0));
    float2 coc = cocTexture.sample(linearSampler, uv).rg;

    float3 farColor = float3(0.0);
    float farWeight = 0.0;

    // Concentric disc sampling for far blur
    float blurRadius = coc.r * uniforms.blurRadiusFar;
    int rings = max(uniforms.blurRings, 1);
    int ringPoints = max(uniforms.blurRingPoints, 1);

    for (int ring = 1; ring <= rings; ring++) {
        float ringRadius = float(ring) / float(rings);
        int pointsInRing = ring * ringPoints;
        for (int p = 0; p < pointsInRing; p++) {
            float angle = float(p) * 6.283185 / float(pointsInRing);
            float2 offset = float2(cos(angle), sin(angle)) * ringRadius * blurRadius;
            float2 sampleUV = uv + offset * uniforms.invResolution;
            sampleUV = clamp(sampleUV, float2(0.0), float2(1.0));
            float sampleCoc = cocTexture.sample(linearSampler, sampleUV).r;
            float w = sampleCoc;  // weight by CoC at sample location
            farColor += farTexture.sample(linearSampler, sampleUV).rgb * w;
            farWeight += w;
        }
    }

    if (farWeight > 0.0) {
        farColor /= farWeight;
    } else {
        farColor = farTexture.sample(linearSampler, uv).rgb;
    }

    // Near blur (optional, blended when CoC near channel > 0)
    float3 result = farColor;
    if (coc.g > 0.0) {
        float3 nearColor = float3(0.0);
        float nearWeight = 0.0;
        float nearBlurRadius = coc.g * uniforms.blurRadiusNear;

        for (int ring = 1; ring <= rings; ring++) {
            float ringRadius = float(ring) / float(rings);
            int pointsInRing = ring * ringPoints;
            for (int p = 0; p < pointsInRing; p++) {
                float angle = float(p) * 6.283185 / float(pointsInRing);
                float2 offset = float2(cos(angle), sin(angle)) * ringRadius * nearBlurRadius;
                float2 sampleUV = uv + offset * uniforms.invResolution;
                sampleUV = clamp(sampleUV, float2(0.0), float2(1.0));
                float sampleCocNear = cocTexture.sample(linearSampler, sampleUV).g;
                float w = sampleCocNear;
                nearColor += nearTexture.sample(linearSampler, sampleUV).rgb * w;
                nearWeight += w;
            }
        }

        if (nearWeight > 0.0) {
            nearColor /= nearWeight;
            result = mix(result, nearColor, coc.g);
        }
    }

    return float4(result, 1.0);
}
)";
    }

    MetalDofBlurPass::MetalDofBlurPass(MetalGraphicsDevice* device, MetalComposePass* composePass)
        : _device(device), _composePass(composePass)
    {
    }

    MetalDofBlurPass::~MetalDofBlurPass()
    {
        if (_depthStencilState) {
            _depthStencilState->release();
            _depthStencilState = nullptr;
        }
    }

    void MetalDofBlurPass::ensureResources()
    {
        // Ensure the compose pass's shared vertex buffer/format are created first
        _composePass->ensureResources();

        if (_shader && _composePass->vertexBuffer() && _composePass->vertexFormat() &&
            _blendState && _depthState && _depthStencilState) {
            return;
        }

        if (!_shader) {
            ShaderDefinition definition;
            definition.name = "DofBlurPass";
            definition.vshader = "dofBlurVertex";
            definition.fshader = "dofBlurFragment";
            _shader = createShader(_device, definition, DOF_BLUR_SOURCE);
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

    void MetalDofBlurPass::execute(MTL::RenderCommandEncoder* encoder,
        const DofBlurPassParams& params,
        MetalRenderPipeline* pipeline, const std::shared_ptr<RenderTarget>& renderTarget,
        const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats,
        MTL::SamplerState* defaultSampler, MTL::DepthStencilState* defaultDepthStencilState)
    {
        if (!encoder || !params.farTexture || !params.cocTexture) {
            return;
        }

        ensureResources();
        if (!_shader || !_composePass->vertexBuffer() || !_composePass->vertexFormat() || !_blendState || !_depthState) {
            spdlog::warn("[executeDofBlurPass] missing DOF blur resources");
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
            spdlog::warn("[executeDofBlurPass] failed to get pipeline state");
            return;
        }

        auto* vb = dynamic_cast<MetalVertexBuffer*>(_composePass->vertexBuffer().get());
        if (!vb || !vb->raw()) {
            spdlog::warn("[executeDofBlurPass] missing vertex buffer");
            return;
        }

        encoder->setRenderPipelineState(pipelineState);
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setDepthStencilState(_depthStencilState ? _depthStencilState : defaultDepthStencilState);
        encoder->setVertexBuffer(vb->raw(), 0, 0);

        // Bind textures: slot 0 = far, slot 1 = CoC, slot 2 = near
        auto* farHw = dynamic_cast<gpu::MetalTexture*>(params.farTexture->impl());
        encoder->setFragmentTexture(farHw ? farHw->raw() : nullptr, 0);

        auto* cocHw = dynamic_cast<gpu::MetalTexture*>(params.cocTexture->impl());
        encoder->setFragmentTexture(cocHw ? cocHw->raw() : nullptr, 1);

        if (params.nearTexture) {
            auto* nearHw = dynamic_cast<gpu::MetalTexture*>(params.nearTexture->impl());
            encoder->setFragmentTexture(nearHw ? nearHw->raw() : nullptr, 2);
        }

        if (defaultSampler) {
            encoder->setFragmentSamplerState(defaultSampler, 0);
        }

        // DofBlurUniforms — float2 invResolution has 8-byte alignment, so pad after blurRadiusFar.
        struct alignas(16) DofBlurUniforms
        {
            float blurRadiusNear;       // offset 0
            float blurRadiusFar;        // offset 4
            float invResolution[2];     // offset 8  (matches Metal float2)
            int32_t blurRings;          // offset 16
            int32_t blurRingPoints;     // offset 20
        } uniforms{};

        uniforms.blurRadiusNear = params.blurRadiusNear;
        uniforms.blurRadiusFar = params.blurRadiusFar;
        uniforms.invResolution[0] = params.invResolutionX;
        uniforms.invResolution[1] = params.invResolutionY;
        uniforms.blurRings = params.blurRings;
        uniforms.blurRingPoints = params.blurRingPoints;
        encoder->setFragmentBytes(&uniforms, sizeof(DofBlurUniforms), 5);

        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, static_cast<NS::UInteger>(0),
            static_cast<NS::UInteger>(3));
        _device->recordDrawCall();
    }
}
