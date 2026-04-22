// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#include "metalEquirectToCubePass.h"

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
        // Face mapping matches the historical CPU faceUvToDir helper so the
        // cubemap layout remains compatible with the convolve-pass X-flip.
        constexpr const char* EQUIRECT_TO_CUBE_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

constant float PI = 3.141592653589793;

struct FaceVertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv0      [[attribute(2)]];
    float4 tangent  [[attribute(3)]];
    float2 uv1      [[attribute(4)]];
};

struct FaceVarying {
    float4 position [[position]];
    float2 uv;
};

struct FaceUniforms {
    uint face;
    uint decodeSrgb;
    uint _pad0;
    uint _pad1;
};

vertex FaceVarying faceVertex(FaceVertexIn in [[stage_in]])
{
    FaceVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

static float3 faceUvToDir(uint face, float2 uv)
{
    const float sc = uv.x * 2.0 - 1.0;
    const float tc = uv.y * 2.0 - 1.0;
    float3 dir;
    if      (face == 0u) dir = float3( 1.0, -tc, -sc);
    else if (face == 1u) dir = float3(-1.0, -tc,  sc);
    else if (face == 2u) dir = float3( sc,   1.0, tc);
    else if (face == 3u) dir = float3( sc,  -1.0,-tc);
    else if (face == 4u) dir = float3( sc,  -tc,  1.0);
    else                 dir = float3(-sc,  -tc, -1.0);
    return normalize(dir);
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

fragment float4 faceFragment(
    FaceVarying in [[stage_in]],
    texture2d<float> sourceEquirect [[texture(0)]],
    sampler          linearSampler  [[sampler(0)]],
    constant FaceUniforms& u        [[buffer(5)]])
{
    const float2 uv = clamp(in.uv, float2(0.0), float2(1.0));
    const float3 dir = faceUvToDir(u.face, uv);
    const float2 eqUv = dirToUvEquirect(dir);
    const float4 raw = sourceEquirect.sample(linearSampler, eqUv);
    const float3 color = (u.decodeSrgb != 0u) ? decodeGammaSrgb(raw) : raw.rgb;
    return float4(color, 1.0);
}
)";
    }

    MetalEquirectToCubePass::MetalEquirectToCubePass(MetalGraphicsDevice* device, MetalComposePass* composePass)
        : _device(device), _composePass(composePass)
    {
    }

    MetalEquirectToCubePass::~MetalEquirectToCubePass()
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

    void MetalEquirectToCubePass::ensureResources()
    {
        _composePass->ensureResources();

        if (_shader && _composePass->vertexBuffer() && _composePass->vertexFormat() &&
            _blendState && _depthState && _depthStencilState && _clampSampler) {
            return;
        }

        if (!_shader) {
            ShaderDefinition definition;
            definition.name = "EquirectToCubePass";
            definition.vshader = "faceVertex";
            definition.fshader = "faceFragment";
            _shader = createShader(_device, definition, EQUIRECT_TO_CUBE_SOURCE);
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
            samplerDesc->setSAddressMode(MTL::SamplerAddressModeRepeat);
            samplerDesc->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
            samplerDesc->setRAddressMode(MTL::SamplerAddressModeClampToEdge);
            _clampSampler = _device->raw()->newSamplerState(samplerDesc);
            samplerDesc->release();
        }
    }

    void MetalEquirectToCubePass::beginPass(MTL::RenderCommandEncoder* encoder,
        Texture* sourceEquirect,
        MetalRenderPipeline* pipeline, const std::shared_ptr<RenderTarget>& renderTarget,
        const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats)
    {
        if (!encoder || !sourceEquirect) return;

        ensureResources();
        if (!_shader || !_composePass->vertexBuffer() || !_composePass->vertexFormat() ||
            !_blendState || !_depthState) {
            spdlog::warn("[MetalEquirectToCubePass] missing resources");
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
            spdlog::warn("[MetalEquirectToCubePass] failed to get pipeline state");
            return;
        }

        auto* vb = dynamic_cast<MetalVertexBuffer*>(_composePass->vertexBuffer().get());
        if (!vb || !vb->raw()) {
            spdlog::warn("[MetalEquirectToCubePass] missing vertex buffer");
            return;
        }

        encoder->setRenderPipelineState(pipelineState);
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setDepthStencilState(_depthStencilState);
        encoder->setVertexBuffer(vb->raw(), 0, 0);

        auto* eqHw = dynamic_cast<gpu::MetalTexture*>(sourceEquirect->impl());
        encoder->setFragmentTexture(eqHw ? eqHw->raw() : nullptr, 0);

        if (_clampSampler) {
            encoder->setFragmentSamplerState(_clampSampler, 0);
        }
    }

    void MetalEquirectToCubePass::drawFace(MTL::RenderCommandEncoder* encoder,
        uint32_t face, int faceSize, bool decodeSrgb)
    {
        if (!encoder || faceSize <= 0) return;

        MTL::Viewport viewport;
        viewport.originX = 0.0;
        viewport.originY = 0.0;
        viewport.width   = static_cast<double>(faceSize);
        viewport.height  = static_cast<double>(faceSize);
        viewport.znear = 0.0;
        viewport.zfar  = 1.0;
        encoder->setViewport(viewport);

        MTL::ScissorRect scissor{0, 0,
            static_cast<NS::UInteger>(faceSize),
            static_cast<NS::UInteger>(faceSize)};
        encoder->setScissorRect(scissor);

        struct alignas(16) FaceUniforms
        {
            uint32_t face;
            uint32_t decodeSrgb;
            uint32_t _pad0;
            uint32_t _pad1;
        } uniforms{};
        uniforms.face = face;
        uniforms.decodeSrgb = decodeSrgb ? 1u : 0u;

        encoder->setFragmentBytes(&uniforms, sizeof(FaceUniforms), 5);

        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
            static_cast<NS::UInteger>(0), static_cast<NS::UInteger>(3));
        _device->recordDrawCall();
    }
}
