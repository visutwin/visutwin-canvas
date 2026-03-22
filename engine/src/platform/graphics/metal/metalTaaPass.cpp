// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// TAA (Temporal Anti-Aliasing) resolve pass implementation.
// Extracted from MetalGraphicsDevice.
//
#include "metalTaaPass.h"

#include "metalComposePass.h"
#include "metalGraphicsDevice.h"
#include "metalRenderPipeline.h"
#include "metalTexture.h"
#include "metalUtils.h"
#include "metalVertexBuffer.h"
#include "core/math/matrix4.h"
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
        constexpr const char* TAA_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

struct ComposeVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv0 [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float2 uv1 [[attribute(4)]];
};

struct TaaVarying {
    float4 position [[position]];
    float2 uv;
};

struct TaaUniforms {
    float4x4 viewProjectionPrevious;
    float4x4 viewProjectionInverse;
    float4 jitters;
    float2 textureSize;
    float4 cameraParams;
    uint highQuality;
    uint historyValid;
};

vertex TaaVarying taaVertex(ComposeVertexIn in [[stage_in]])
{
    TaaVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

static inline float linearizeDepth(float z, float4 cameraParams)
{
    if (cameraParams.w == 0.0) {
        return (cameraParams.z * cameraParams.y) / (cameraParams.y + z * (cameraParams.z - cameraParams.y));
    }
    return cameraParams.z + z * (cameraParams.y - cameraParams.z);
}

static inline float delinearizeDepth(float linearDepth, float4 cameraParams)
{
    if (cameraParams.w == 0.0) {
        return (cameraParams.y * (cameraParams.z - linearDepth)) /
            (linearDepth * (cameraParams.z - cameraParams.y));
    }
    return (linearDepth - cameraParams.z) / (cameraParams.y - cameraParams.z);
}

static inline float2 reproject(float2 uv, float depth, constant TaaUniforms& uniforms)
{
    // DEVIATION: Metal depth buffer stores (ndcZ_gl + 1) / 2, undo to get OpenGL NDC Z
    depth = depth * 2.0 - 1.0;

    // DEVIATION: UV has Metal convention (V=0 at top), but the projection matrix uses
    // OpenGL convention (NDC Y=+1 at top). Convert: ndcX = uv.x*2-1, ndcY = (1-uv.y)*2-1 = 1-2*uv.y.
    float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y, depth, 1.0);

    // Remove jitter from the current frame
    ndc.xy -= uniforms.jitters.xy;

    float4 worldPosition = uniforms.viewProjectionInverse * ndc;
    worldPosition /= worldPosition.w;

    float4 screenPrevious = uniforms.viewProjectionPrevious * worldPosition;
    // Convert back from NDC to Metal UV convention (flip Y back)
    float2 prevNdc = screenPrevious.xy / screenPrevious.w;
    return float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
}

static inline float4 SampleTextureCatmullRom(
    texture2d<float> tex, sampler linearSampler, float2 uv, float2 texSize)
{
    float2 samplePos = uv * texSize;
    float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - texPos1;

    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);

    float2 w12 = w1 + w2;
    float2 offset12 = w2 / (w1 + w2);

    float2 texPos0 = (texPos1 - 1.0) / texSize;
    float2 texPos3 = (texPos1 + 2.0) / texSize;
    float2 texPos12 = (texPos1 + offset12) / texSize;

    float4 result = float4(0.0);
    result += tex.sample(linearSampler, float2(texPos0.x, texPos0.y), level(0.0)) * w0.x * w0.y;
    result += tex.sample(linearSampler, float2(texPos12.x, texPos0.y), level(0.0)) * w12.x * w0.y;
    result += tex.sample(linearSampler, float2(texPos3.x, texPos0.y), level(0.0)) * w3.x * w0.y;

    result += tex.sample(linearSampler, float2(texPos0.x, texPos12.y), level(0.0)) * w0.x * w12.y;
    result += tex.sample(linearSampler, float2(texPos12.x, texPos12.y), level(0.0)) * w12.x * w12.y;
    result += tex.sample(linearSampler, float2(texPos3.x, texPos12.y), level(0.0)) * w3.x * w12.y;

    result += tex.sample(linearSampler, float2(texPos0.x, texPos3.y), level(0.0)) * w0.x * w3.y;
    result += tex.sample(linearSampler, float2(texPos12.x, texPos3.y), level(0.0)) * w12.x * w3.y;
    result += tex.sample(linearSampler, float2(texPos3.x, texPos3.y), level(0.0)) * w3.x * w3.y;
    return result;
}

static inline float4 colorClamp(texture2d<float> sourceTexture, sampler linearSampler, float2 uv, float4 historyColor, float2 textureSize)
{
    float3 minColor = float3(9999.0);
    float3 maxColor = float3(-9999.0);
    for (float x = -1.0; x <= 1.0; ++x) {
        for (float y = -1.0; y <= 1.0; ++y) {
            float3 color = sourceTexture.sample(linearSampler, uv + float2(x, y) / textureSize).rgb;
            minColor = min(minColor, color);
            maxColor = max(maxColor, color);
        }
    }

    float3 clamped = clamp(historyColor.rgb, minColor, maxColor);
    return float4(clamped, historyColor.a);
}

fragment float4 taaFragment(
    TaaVarying in [[stage_in]],
    texture2d<float> sourceTexture [[texture(0)]],
    texture2d<float> historyTexture [[texture(1)]],
    depth2d<float> depthTexture [[texture(2)]],
    sampler linearSampler [[sampler(0)]],
    constant TaaUniforms& uniforms [[buffer(5)]])
{
    // TAA resolve (GLSL to Metal).
    const float2 uv = clamp(in.uv, float2(0.0), float2(1.0));

    // Current frame color
    const float4 srcColor = sourceTexture.sample(linearSampler, uv);

    // If no valid history, just pass through current frame
    if (uniforms.historyValid == 0u) {
        return srcColor;
    }

    // DEVIATION: upstream uses getLinearScreenDepth()/delinearizeDepth() from
    // screenDepthPS for the round-trip; the linearize->delinearize is an identity
    // on the raw hardware depth.  We skip the round-trip and use rawDepth directly
    // since reproject() only needs the original viewport [0,1] depth.
    float depth = depthTexture.sample(linearSampler, uv);

    // Reproject: find where this pixel was in the previous frame
    float2 historyUv = reproject(uv, depth, uniforms);

    // Sample history: Catmull-Rom (high quality) or bilinear
    float4 historyColor;
    if (uniforms.highQuality != 0u) {
        historyColor = SampleTextureCatmullRom(historyTexture, linearSampler, historyUv, uniforms.textureSize);
    } else {
        historyColor = historyTexture.sample(linearSampler, historyUv);
    }

    // Color clamping to handle disocclusion
    float4 historyColorClamped = colorClamp(sourceTexture, linearSampler, uv, historyColor, uniforms.textureSize);

    // Reject history samples that project outside the frame
    float mixFactor = (historyUv.x < 0.0 || historyUv.x > 1.0 ||
                       historyUv.y < 0.0 || historyUv.y > 1.0) ? 1.0 : 0.05;

    return mix(historyColorClamped, srcColor, mixFactor);
}
)";
    }

    MetalTaaPass::MetalTaaPass(MetalGraphicsDevice* device, MetalComposePass* composePass)
        : _device(device), _composePass(composePass)
    {
    }

    MetalTaaPass::~MetalTaaPass()
    {
        if (_depthStencilState) {
            _depthStencilState->release();
            _depthStencilState = nullptr;
        }
    }

    void MetalTaaPass::ensureResources()
    {
        if (_shader && _composePass->vertexBuffer() && _composePass->vertexFormat() &&
            _blendState && _depthState && _depthStencilState) {
            return;
        }

        if (!_shader) {
            ShaderDefinition definition;
            definition.name = "TaaResolvePass";
            definition.vshader = "taaVertex";
            definition.fshader = "taaFragment";
            _shader = createShader(_device, definition, TAA_SOURCE);
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

    void MetalTaaPass::execute(MTL::RenderCommandEncoder* encoder,
        Texture* sourceTexture, Texture* historyTexture, Texture* depthTexture,
        const Matrix4& viewProjectionPrevious, const Matrix4& viewProjectionInverse,
        const std::array<float, 4>& jitters, const std::array<float, 4>& cameraParams,
        const bool highQuality, const bool historyValid,
        MetalRenderPipeline* pipeline, const std::shared_ptr<RenderTarget>& renderTarget,
        const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats,
        MTL::SamplerState* defaultSampler, MTL::DepthStencilState* defaultDepthStencilState)
    {
        if (!encoder || !sourceTexture || !historyTexture || !depthTexture) {
            return;
        }

        ensureResources();
        if (!_shader || !_composePass->vertexBuffer() || !_composePass->vertexFormat() || !_blendState || !_depthState) {
            spdlog::warn("[executeTAAPass] missing TAA resources");
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
            spdlog::warn("[executeTAAPass] failed to get pipeline state");
            return;
        }

        auto* vb = dynamic_cast<MetalVertexBuffer*>(_composePass->vertexBuffer().get());
        if (!vb || !vb->raw()) {
            spdlog::warn("[executeTAAPass] missing vertex buffer");
            return;
        }

        encoder->setRenderPipelineState(pipelineState);
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setDepthStencilState(_depthStencilState ? _depthStencilState : defaultDepthStencilState);
        encoder->setVertexBuffer(vb->raw(), 0, 0);

        auto* sourceHw = dynamic_cast<gpu::MetalTexture*>(sourceTexture->impl());
        auto* historyHw = dynamic_cast<gpu::MetalTexture*>(historyTexture->impl());
        auto* depthHw = dynamic_cast<gpu::MetalTexture*>(depthTexture->impl());

        encoder->setFragmentTexture(sourceHw ? sourceHw->raw() : nullptr, 0);
        encoder->setFragmentTexture(historyHw ? historyHw->raw() : nullptr, 1);
        encoder->setFragmentTexture(depthHw ? depthHw->raw() : nullptr, 2);
        if (defaultSampler) {
            encoder->setFragmentSamplerState(defaultSampler, 0);
        }

        struct alignas(16) TaaUniforms
        {
            simd::float4x4 viewProjectionPrevious;
            simd::float4x4 viewProjectionInverse;
            simd::float4 jitters;
            simd::float2 textureSize;
            simd::float4 cameraParams;
            uint32_t highQuality;
            uint32_t historyValid;
        } uniforms{};
        uniforms.viewProjectionPrevious = metal::toSimdMatrix(viewProjectionPrevious);
        uniforms.viewProjectionInverse = metal::toSimdMatrix(viewProjectionInverse);

        uniforms.jitters = simd::float4{jitters[0], jitters[1], jitters[2], jitters[3]};
        uniforms.textureSize = simd::float2{
            static_cast<float>(std::max(sourceTexture->width(), 1u)),
            static_cast<float>(std::max(sourceTexture->height(), 1u))
        };
        uniforms.cameraParams = simd::float4{cameraParams[0], cameraParams[1], cameraParams[2], cameraParams[3]};
        uniforms.highQuality = highQuality ? 1u : 0u;
        uniforms.historyValid = historyValid ? 1u : 0u;
        encoder->setFragmentBytes(&uniforms, sizeof(TaaUniforms), 5);

        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, static_cast<NS::UInteger>(0),
            static_cast<NS::UInteger>(3));
        _device->recordDrawCall();
    }
}
