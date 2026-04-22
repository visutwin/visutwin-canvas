// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#include "metalEnvConvolvePass.h"

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
        constexpr const char* CONVOLVE_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

constant float PI = 3.141592653589793;

struct ConvolveVertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv0      [[attribute(2)]];
    float4 tangent  [[attribute(3)]];
    float2 uv1      [[attribute(4)]];
};

struct ConvolveVarying {
    float4 position [[position]];
    float2 vUv;
};

struct ConvolveUniforms {
    float4 uvMod;
    uint sourceIsCubemap;
    uint encodeRgbp;
    uint decodeSrgb;
    uint numSamples;
};

vertex ConvolveVarying convolveVertex(ConvolveVertexIn in [[stage_in]],
                                      constant ConvolveUniforms& u [[buffer(5)]])
{
    ConvolveVarying out;
    out.position = float4(in.position, 1.0);
    out.vUv = in.uv0 * u.uvMod.xy + u.uvMod.zw;
    return out;
}

static float3 uvToDirEquirect(float2 vUv)
{
    const float phi   = (vUv.x * 2.0 - 1.0) * PI;
    const float theta = (1.0 - vUv.y) * PI - PI * 0.5;
    const float c = cos(theta);
    return float3(sin(phi) * c, sin(theta), cos(phi) * c);
}

static float2 dirToUvEquirect(float3 dir)
{
    const float phi   = atan2(dir.x, dir.z);
    const float theta = asin(clamp(dir.y, -1.0, 1.0));
    return float2(phi / (2.0 * PI) + 0.5, 1.0 - (theta / PI + 0.5));
}

static float3 decodeGammaSrgb(float4 raw)
{
    return pow(max(raw.rgb, float3(0.0)), float3(2.2));
}

static float4 packRgbp(float3 color)
{
    const float3 lin = max(color, float3(0.0));
    const float sr = sqrt(lin.r);
    const float sg = sqrt(lin.g);
    const float sb = sqrt(lin.b);
    const float maxVal = max(max(sr, sg), max(sb, 1.0 / 255.0));
    const float a = clamp((8.0 - maxVal) / 7.0, 0.0, 1.0);
    const float scale = -a * 7.0 + 8.0;
    return float4(clamp(float3(sr, sg, sb) / scale, 0.0, 1.0), a);
}

fragment float4 convolveFragment(
    ConvolveVarying in [[stage_in]],
    texture2d<float>   sourceEquirect [[texture(0)]],
    texturecube<float> sourceCubemap  [[texture(1)]],
    sampler            linearSampler  [[sampler(0)]],
    constant ConvolveUniforms& u      [[buffer(5)]],
    constant float4* samples          [[buffer(6)]])
{
    const float3 N = normalize(uvToDirEquirect(in.vUv));

    float3 up = (abs(N.y) > 0.999) ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
    float3 T = normalize(cross(up, N));
    float3 B = cross(N, T);

    float3 sum = float3(0.0);
    float weight = 0.0;

    for (uint i = 0u; i < u.numSamples; ++i) {
        const float4 s = samples[i];
        if (s.z <= 0.0) {
            continue;
        }
        const float3 L = T * s.x + B * s.y + N * s.z;
        const float mip = s.w;

        float3 color;
        if (u.sourceIsCubemap != 0u) {
            float3 cubeDir = L;
            cubeDir.x = -cubeDir.x;
            color = sourceCubemap.sample(linearSampler, cubeDir, level(mip)).rgb;
        } else {
            const float2 srcUv = dirToUvEquirect(L);
            color = sourceEquirect.sample(linearSampler, srcUv, level(mip)).rgb;
        }
        if (u.decodeSrgb != 0u) {
            color = decodeGammaSrgb(float4(color, 1.0));
        }
        sum += color;
        weight += 1.0;
    }

    sum /= max(weight, 1.0);

    if (u.encodeRgbp != 0u) {
        return packRgbp(sum);
    }
    return float4(sum, 1.0);
}
)";
    }

    MetalEnvConvolvePass::MetalEnvConvolvePass(MetalGraphicsDevice* device, MetalComposePass* composePass)
        : _device(device), _composePass(composePass)
    {
    }

    MetalEnvConvolvePass::~MetalEnvConvolvePass()
    {
        if (_depthStencilState) {
            _depthStencilState->release();
            _depthStencilState = nullptr;
        }
        if (_clampSampler) {
            _clampSampler->release();
            _clampSampler = nullptr;
        }
    }

    void MetalEnvConvolvePass::ensureResources()
    {
        _composePass->ensureResources();

        if (_shader && _composePass->vertexBuffer() && _composePass->vertexFormat() &&
            _blendState && _depthState && _depthStencilState && _clampSampler) {
            return;
        }

        if (!_shader) {
            ShaderDefinition definition;
            definition.name = "EnvConvolvePass";
            definition.vshader = "convolveVertex";
            definition.fshader = "convolveFragment";
            _shader = createShader(_device, definition, CONVOLVE_SOURCE);
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
        if (!_clampSampler && _device->raw()) {
            auto* samplerDesc = MTL::SamplerDescriptor::alloc()->init();
            samplerDesc->setMinFilter(MTL::SamplerMinMagFilterLinear);
            samplerDesc->setMagFilter(MTL::SamplerMinMagFilterLinear);
            samplerDesc->setMipFilter(MTL::SamplerMipFilterLinear);
            samplerDesc->setSAddressMode(MTL::SamplerAddressModeRepeat);
            samplerDesc->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
            samplerDesc->setRAddressMode(MTL::SamplerAddressModeClampToEdge);
            _clampSampler = _device->raw()->newSamplerState(samplerDesc);
            samplerDesc->release();
        }
    }

    void MetalEnvConvolvePass::beginPass(MTL::RenderCommandEncoder* encoder,
        Texture* sourceEquirect, Texture* sourceCubemap,
        MetalRenderPipeline* pipeline, const std::shared_ptr<RenderTarget>& renderTarget,
        const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats)
    {
        if (!encoder) return;
        if (!sourceEquirect && !sourceCubemap) {
            spdlog::warn("[MetalEnvConvolvePass] no source texture provided");
            return;
        }

        ensureResources();
        if (!_shader || !_composePass->vertexBuffer() || !_composePass->vertexFormat() ||
            !_blendState || !_depthState) {
            spdlog::warn("[MetalEnvConvolvePass] missing resources");
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
            spdlog::warn("[MetalEnvConvolvePass] failed to get pipeline state");
            return;
        }

        auto* vb = dynamic_cast<MetalVertexBuffer*>(_composePass->vertexBuffer().get());
        if (!vb || !vb->raw()) {
            spdlog::warn("[MetalEnvConvolvePass] missing vertex buffer");
            return;
        }

        encoder->setRenderPipelineState(pipelineState);
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setDepthStencilState(_depthStencilState);
        encoder->setVertexBuffer(vb->raw(), 0, 0);

        auto* eqHw = sourceEquirect
            ? dynamic_cast<gpu::MetalTexture*>(sourceEquirect->impl()) : nullptr;
        auto* cubeHw = sourceCubemap
            ? dynamic_cast<gpu::MetalTexture*>(sourceCubemap->impl()) : nullptr;

        encoder->setFragmentTexture(eqHw ? eqHw->raw() : nullptr, 0);
        encoder->setFragmentTexture(cubeHw ? cubeHw->raw() : nullptr, 1);

        if (_clampSampler) {
            encoder->setFragmentSamplerState(_clampSampler, 0);
        }
    }

    void MetalEnvConvolvePass::drawRect(MTL::RenderCommandEncoder* encoder,
        const EnvConvolveOp& op, bool sourceIsCubemap, bool encodeRgbp, bool decodeSrgb)
    {
        if (!encoder) return;
        if (op.rectW <= 0 || op.rectH <= 0) return;
        if (!op.samples || op.numSamples <= 0) {
            spdlog::warn("[MetalEnvConvolvePass] op has no samples");
            return;
        }

        MTL::Viewport viewport;
        viewport.originX = static_cast<double>(op.rectX);
        viewport.originY = static_cast<double>(op.rectY);
        viewport.width   = static_cast<double>(op.rectW);
        viewport.height  = static_cast<double>(op.rectH);
        viewport.znear = 0.0;
        viewport.zfar  = 1.0;
        encoder->setViewport(viewport);

        MTL::ScissorRect scissor;
        scissor.x = static_cast<NS::UInteger>(std::max(0, op.rectX));
        scissor.y = static_cast<NS::UInteger>(std::max(0, op.rectY));
        scissor.width  = static_cast<NS::UInteger>(std::max(1, op.rectW));
        scissor.height = static_cast<NS::UInteger>(std::max(1, op.rectH));
        encoder->setScissorRect(scissor);

        const size_t samplesBytes = static_cast<size_t>(op.numSamples) * 4 * sizeof(float);
        auto* samplesBuffer = _device->raw()->newBuffer(
            op.samples, samplesBytes, MTL::ResourceStorageModeShared);
        if (!samplesBuffer) {
            spdlog::error("[MetalEnvConvolvePass] failed to allocate samples buffer");
            return;
        }
        encoder->setFragmentBuffer(samplesBuffer, 0, 6);
        // Release our reference; the encoder retains the buffer until the
        // command buffer completes.
        samplesBuffer->release();

        struct alignas(16) ConvolveUniforms
        {
            float uvMod[4];
            uint32_t sourceIsCubemap;
            uint32_t encodeRgbp;
            uint32_t decodeSrgb;
            uint32_t numSamples;
        } uniforms{};

        const int seam = std::max(0, op.seamPixels);
        const int innerW = op.rectW - seam * 2;
        const int innerH = op.rectH - seam * 2;
        if (seam > 0 && innerW > 0 && innerH > 0) {
            uniforms.uvMod[0] = static_cast<float>(innerW + seam * 2) / static_cast<float>(innerW);
            uniforms.uvMod[1] = static_cast<float>(innerH + seam * 2) / static_cast<float>(innerH);
            uniforms.uvMod[2] = -static_cast<float>(seam) / static_cast<float>(innerW);
            uniforms.uvMod[3] = -static_cast<float>(seam) / static_cast<float>(innerH);
        } else {
            uniforms.uvMod[0] = 1.0f;
            uniforms.uvMod[1] = 1.0f;
            uniforms.uvMod[2] = 0.0f;
            uniforms.uvMod[3] = 0.0f;
        }
        uniforms.sourceIsCubemap = sourceIsCubemap ? 1u : 0u;
        uniforms.encodeRgbp = encodeRgbp ? 1u : 0u;
        uniforms.decodeSrgb = decodeSrgb ? 1u : 0u;
        uniforms.numSamples = static_cast<uint32_t>(op.numSamples);

        encoder->setVertexBytes(&uniforms, sizeof(ConvolveUniforms), 5);
        encoder->setFragmentBytes(&uniforms, sizeof(ConvolveUniforms), 5);

        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
            static_cast<NS::UInteger>(0), static_cast<NS::UInteger>(3));
        _device->recordDrawCall();
    }
}
