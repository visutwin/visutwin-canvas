// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Surface LIC post-processing pass implementation.
// Follows the MetalTaaPass decomposition pattern.
//
#include "metalLICPass.h"

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
        // ── Embedded Metal Shading Language source ───────────────────
        //
        // Image-space LIC fragment shader.
        //
        // For each pixel:
        //   1. Read screen-space velocity from velocity texture (RG16Float)
        //   2. Trace forward L steps through the velocity field
        //   3. Trace backward L steps
        //   4. Accumulate noise texture samples along the streamline
        //   5. Normalize → grayscale LIC value
        //   6. Apply contrast enhancement
        //
        // Velocity texture convention:
        //   RG = (vx, vy) in normalized texture coordinates per step
        //   (i.e., a velocity of (1, 0) moves 1 texel per integration step)
        //
        // Noise texture:
        //   R8Unorm, tiled across the screen.  Animation phase offsets
        //   the noise sampling for animated LIC (OLIC).
        //
        constexpr const char* LIC_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

struct LICVertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv0      [[attribute(2)]];
    float4 tangent  [[attribute(3)]];
    float2 uv1      [[attribute(4)]];
};

struct LICVarying {
    float4 position [[position]];
    float2 uv;
};

struct LICUniforms {
    float2 textureSize;     // Velocity texture dimensions (pixels)
    float2 noiseSize;       // Noise texture dimensions (pixels)
    float  stepSize;        // Step size in normalized tex coords
    float  animationPhase;  // Phase offset [0, 1] for animated LIC
    int    integrationSteps;// L: steps in each direction
    float  contrastLo;      // Low end of contrast range
    float  contrastHi;      // High end of contrast range
    float  minVelocity;     // Stagnation threshold
};

vertex LICVarying licVertex(LICVertexIn in [[stage_in]])
{
    LICVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

fragment float4 licFragment(
    LICVarying in [[stage_in]],
    texture2d<float> velocityTexture [[texture(0)]],
    texture2d<float> noiseTexture    [[texture(1)]],
    sampler linearSampler            [[sampler(0)]],
    constant LICUniforms& uniforms   [[buffer(5)]])
{
    const float2 uv = clamp(in.uv, float2(0.0), float2(1.0));

    // Read velocity at this pixel (in normalized texture coordinates)
    const float2 vel = velocityTexture.sample(linearSampler, uv).rg;
    const float speed = length(vel);

    // Stagnation → neutral gray
    if (speed < uniforms.minVelocity) {
        return float4(0.5, 0.5, 0.5, 1.0);
    }

    // Normalized velocity direction
    const float2 dir = vel / speed;
    const float dt = uniforms.stepSize;

    // Phase offset for animation (shifts noise sampling)
    const float phaseX = uniforms.animationPhase * uniforms.noiseSize.x;

    // Accumulate noise along the streamline
    float sum = 0.0;
    float weightSum = 0.0;

    // Center sample
    float2 noiseUV = uv * uniforms.textureSize / uniforms.noiseSize;
    noiseUV.x += phaseX / uniforms.noiseSize.x;
    float n = noiseTexture.sample(linearSampler, noiseUV).r;
    sum += n;
    weightSum += 1.0;

    // Forward trace
    float2 pos = uv;
    for (int i = 0; i < uniforms.integrationSteps; ++i) {
        float2 v = velocityTexture.sample(linearSampler, pos).rg;
        float s = length(v);
        if (s < uniforms.minVelocity) break;

        // Normalize and step
        float2 d = v / s;
        pos += d * dt;

        // Bounds check
        if (pos.x < 0.0 || pos.x > 1.0 || pos.y < 0.0 || pos.y > 1.0) break;

        // Sample noise (tiled + animated)
        noiseUV = pos * uniforms.textureSize / uniforms.noiseSize;
        noiseUV.x += phaseX / uniforms.noiseSize.x;
        n = noiseTexture.sample(linearSampler, noiseUV).r;
        sum += n;
        weightSum += 1.0;
    }

    // Backward trace
    pos = uv;
    for (int i = 0; i < uniforms.integrationSteps; ++i) {
        float2 v = velocityTexture.sample(linearSampler, pos).rg;
        float s = length(v);
        if (s < uniforms.minVelocity) break;

        float2 d = v / s;
        pos -= d * dt;

        if (pos.x < 0.0 || pos.x > 1.0 || pos.y < 0.0 || pos.y > 1.0) break;

        noiseUV = pos * uniforms.textureSize / uniforms.noiseSize;
        noiseUV.x += phaseX / uniforms.noiseSize.x;
        n = noiseTexture.sample(linearSampler, noiseUV).r;
        sum += n;
        weightSum += 1.0;
    }

    // Normalize
    float lic = (weightSum > 0.0) ? (sum / weightSum) : 0.5;

    // Contrast enhancement: remap from [0,1] to [contrastLo, contrastHi]
    // (simple linear stretch; full histogram equalization done in CPU pass)
    lic = uniforms.contrastLo + lic * (uniforms.contrastHi - uniforms.contrastLo);
    lic = clamp(lic, 0.0, 1.0);

    return float4(lic, lic, lic, 1.0);
}
)";
    } // anonymous namespace

    MetalLICPass::MetalLICPass(MetalGraphicsDevice* device, MetalComposePass* composePass)
        : _device(device), _composePass(composePass)
    {
    }

    MetalLICPass::~MetalLICPass()
    {
        if (_depthStencilState) {
            _depthStencilState->release();
            _depthStencilState = nullptr;
        }
    }

    void MetalLICPass::ensureResources()
    {
        if (_shader && _composePass->vertexBuffer() && _composePass->vertexFormat() &&
            _blendState && _depthState && _depthStencilState) {
            return;
        }

        if (!_shader) {
            ShaderDefinition definition;
            definition.name = "SurfaceLICPass";
            definition.vshader = "licVertex";
            definition.fshader = "licFragment";
            _shader = createShader(_device, definition, LIC_SOURCE);
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

    void MetalLICPass::execute(MTL::RenderCommandEncoder* encoder,
        Texture* velocityTexture,
        Texture* noiseTexture,
        const int integrationSteps,
        const float stepSize,
        const float animationPhase,
        const float contrastLo,
        const float contrastHi,
        MetalRenderPipeline* pipeline,
        const std::shared_ptr<RenderTarget>& renderTarget,
        const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats,
        MTL::SamplerState* defaultSampler,
        MTL::DepthStencilState* defaultDepthStencilState)
    {
        if (!encoder || !velocityTexture || !noiseTexture) {
            return;
        }

        ensureResources();
        if (!_shader || !_composePass->vertexBuffer() || !_composePass->vertexFormat() ||
            !_blendState || !_depthState) {
            spdlog::warn("[MetalLICPass] missing resources");
            return;
        }

        Primitive primitive;
        primitive.type = PRIMITIVE_TRIANGLES;
        primitive.base = 0;
        primitive.count = 3;
        primitive.indexed = false;

        auto pipelineState = pipeline->get(primitive, _composePass->vertexFormat(), nullptr, -1,
            _shader, renderTarget, bindGroupFormats, _blendState, _depthState,
            CullMode::CULLFACE_NONE, false, nullptr, nullptr);
        if (!pipelineState) {
            spdlog::warn("[MetalLICPass] failed to get pipeline state");
            return;
        }

        auto* vb = dynamic_cast<MetalVertexBuffer*>(_composePass->vertexBuffer().get());
        if (!vb || !vb->raw()) {
            spdlog::warn("[MetalLICPass] missing vertex buffer");
            return;
        }

        encoder->setRenderPipelineState(pipelineState);
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setDepthStencilState(
            _depthStencilState ? _depthStencilState : defaultDepthStencilState);
        encoder->setVertexBuffer(vb->raw(), 0, 0);

        // Bind textures
        auto* velHw = dynamic_cast<gpu::MetalTexture*>(velocityTexture->impl());
        auto* noiseHw = dynamic_cast<gpu::MetalTexture*>(noiseTexture->impl());

        encoder->setFragmentTexture(velHw ? velHw->raw() : nullptr, 0);
        encoder->setFragmentTexture(noiseHw ? noiseHw->raw() : nullptr, 1);
        if (defaultSampler) {
            encoder->setFragmentSamplerState(defaultSampler, 0);
        }

        // Pack uniforms
        struct alignas(16) LICUniforms
        {
            simd::float2 textureSize;
            simd::float2 noiseSize;
            float stepSize;
            float animationPhase;
            int32_t integrationSteps;
            float contrastLo;
            float contrastHi;
            float minVelocity;
        } uniforms{};

        uniforms.textureSize = simd::float2{
            static_cast<float>(std::max(velocityTexture->width(), 1u)),
            static_cast<float>(std::max(velocityTexture->height(), 1u))
        };
        uniforms.noiseSize = simd::float2{
            static_cast<float>(std::max(noiseTexture->width(), 1u)),
            static_cast<float>(std::max(noiseTexture->height(), 1u))
        };
        uniforms.stepSize = stepSize;
        uniforms.animationPhase = animationPhase;
        uniforms.integrationSteps = integrationSteps;
        uniforms.contrastLo = contrastLo;
        uniforms.contrastHi = contrastHi;
        uniforms.minVelocity = 1.0e-6f;

        encoder->setFragmentBytes(&uniforms, sizeof(LICUniforms), 5);

        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
            static_cast<NS::UInteger>(0), static_cast<NS::UInteger>(3));
        _device->recordDrawCall();
    }
}
