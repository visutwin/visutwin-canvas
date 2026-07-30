// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Volumetric fog pass implementation. Shaders ported from upstream
// scene/shader-lib/glsl/chunks/render-pass/frag/volumetricFog.js and volumetricFogCombine.js.
//
#include "metalVolumetricFogPass.h"

#include "metalComposePass.h"
#include "metalGraphicsDevice.h"
#include "metalRenderPipeline.h"
#include "metalTexture.h"
#include "metalVertexBuffer.h"
#include "core/math/matrix4.h"
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
        constexpr const char* VOLUMETRIC_FOG_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

struct ComposeVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv0 [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float2 uv1 [[attribute(4)]];
};

struct FogVarying {
    float4 position [[position]];
    float2 uv;
};

struct FogUniforms {
    float4x4 invView;                 // offset   0
    float4x4 shadowMatrixPalette[4];  // offset  64
    float4 cameraPosition;            // offset 320  xyz
    float4 cameraForward;             // offset 336  xyz
    float4 projScale;                 // offset 352  xy
    float4 tint;                      // offset 368  xyz
    float4 lightColor;                // offset 384  xyz
    float4 lightDirection;            // offset 400  xyz
    float4 ambient;                   // offset 416  xyz
    float4 fogParams;                 // offset 432  x=density y=heightBase z=heightFalloff w=maxDistance
    float4 scatterParams;             // offset 448  x=anisotropy y=steps z=noiseOffset w=shadowIntensity
    float4 shadowCascadeDistances;    // offset 464
    float4 shadowParams;              // offset 480  x=cascadeCount y=bias z=hasShadows w=shadowDistance
    float4 cameraParams;              // offset 496  x=near y=far z=extinction
};

vertex FogVarying fogVertex(ComposeVertexIn in [[stage_in]])
{
    FogVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

static inline float getLinearDepth(float rawDepth, float cameraNear, float cameraFar)
{
    // Standard depth [0,1]: near=0, far=1. Returns positive linear view-space distance.
    return (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
}

// Interleaved gradient noise, used to offset the ray-march samples and hide banding.
static inline float fogNoise(float2 fragCoord)
{
    const float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(fragCoord, magic.xy)));
}

// Normalized Henyey-Greenstein phase function.
static inline float fogPhase(float cosTheta, float g)
{
    const float g2 = g * g;
    const float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (12.56637 * denom * sqrt(max(denom, 1e-6)));
}

// Cascaded directional shadow lookup along the ray. Mirrors the forward pass's cascade
// selection: pick the first cascade whose split distance exceeds the view depth.
static inline float sampleFogShadow(float3 worldPos, float viewDepth,
    constant FogUniforms& u, depth2d<float> shadowMap, sampler shadowSampler)
{
    if (viewDepth >= u.shadowParams.w) {
        return 1.0;
    }

    const float4 comparisons = step(u.shadowCascadeDistances, float4(viewDepth));
    const int cascadeIndex = int(min(dot(comparisons, float4(1.0)), u.shadowParams.x - 1.0));

    const float4 shadowCoord = u.shadowMatrixPalette[cascadeIndex] * float4(worldPos, 1.0);
    if (shadowCoord.w <= 0.0) {
        return 1.0;
    }
    const float3 coord = shadowCoord.xyz / shadowCoord.w;

    // Outside the cascade's atlas region contributes no shadow.
    if (any(coord.xy < float2(0.0)) || any(coord.xy > float2(1.0))) {
        return 1.0;
    }

    return shadowMap.sample_compare(shadowSampler, coord.xy, coord.z - u.shadowParams.y);
}

fragment float4 fogFragment(
    FogVarying in [[stage_in]],
    depth2d<float> depthTexture [[texture(0)]],
    depth2d<float> shadowTexture [[texture(1)]],
    sampler linearSampler [[sampler(0)]],
    constant FogUniforms& u [[buffer(5)]])
{
    const float2 uv = clamp(in.uv, float2(0.0), float2(1.0));

    const float cameraNear = u.cameraParams.x;
    const float cameraFar = u.cameraParams.y;
    const float extinction = u.cameraParams.z;

    // World-space ray for this pixel.
    const float2 ndc = uv * 2.0 - 1.0;
    const float3 rayDir = normalize(
        (u.invView * float4(ndc * u.projScale.xy, -1.0, 0.0)).xyz);

    // Distance along the ray to the scene surface. The depth buffer stores distance along the
    // view axis, so divide by the ray's projection onto the forward vector.
    const float rawDepth = depthTexture.sample(linearSampler, uv);
    const float sceneDepth = getLinearDepth(rawDepth, cameraNear, cameraFar);
    const float rayDot = max(dot(rayDir, u.cameraForward.xyz), 0.001);
    const float rayLength = min(sceneDepth / rayDot, u.fogParams.w);

    const float stepCount = max(u.scatterParams.y, 1.0);
    const float dt = rayLength / stepCount;

    // Per-pixel dither, cycled per frame so TAA can accumulate it away.
    const float noise = fract(fogNoise(in.position.xy) + u.scatterParams.z);

    // The light direction is constant along the ray, so the phase term is evaluated once.
    const float3 sunLight = u.lightColor.xyz *
        fogPhase(dot(rayDir, u.lightDirection.xyz), u.scatterParams.x);

    const bool hasShadows = u.shadowParams.z > 0.5;

    // Metal's comparison sampler must be declared in the shader; depth compare is LESS so that
    // a fragment nearer than the stored caster depth is lit.
    constexpr sampler shadowSampler(coord::normalized, filter::linear,
        address::clamp_to_edge, compare_func::less_equal);

    float3 inscatter = float3(0.0);
    float transmittance = 1.0;

    for (float i = 0.0; i < stepCount; i += 1.0) {
        const float t = (i + noise) * dt;
        const float3 pos = u.cameraPosition.xyz + rayDir * t;

        // Exponential height fog, constant below the base height.
        const float density = u.fogParams.x *
            exp(-u.fogParams.z * max(pos.y - u.fogParams.y, 0.0));

        float shadow = 1.0;
        if (hasShadows) {
            shadow = mix(1.0, sampleFogShadow(pos, t * rayDot, u, shadowTexture, shadowSampler),
                u.scatterParams.w);
        }

        // Accumulate in-scattered light, then attenuate through this slab (Beer-Lambert).
        const float3 radiance = sunLight * shadow + u.ambient.xyz;
        inscatter += transmittance * u.tint.xyz * radiance * (density * dt);
        transmittance *= exp(-extinction * density * dt);

        if (transmittance < 0.005) {
            break;
        }
    }

    return float4(inscatter, transmittance);
}
)";

        constexpr const char* VOLUMETRIC_FOG_COMBINE_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

struct ComposeVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv0 [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float2 uv1 [[attribute(4)]];
};

struct FogCombineVarying {
    float4 position [[position]];
    float2 uv;
};

struct FogCombineUniforms {
    float4 textureSize;   // xy = fog resolution, zw = 1/resolution
    float4 cameraParams;  // x = near, y = far
};

vertex FogCombineVarying fogCombineVertex(ComposeVertexIn in [[stage_in]])
{
    FogCombineVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

static inline float getLinearDepth(float rawDepth, float cameraNear, float cameraFar)
{
    return (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
}

fragment float4 fogCombineFragment(
    FogCombineVarying in [[stage_in]],
    depth2d<float> depthTexture [[texture(0)]],
    texture2d<float> fogTexture [[texture(1)]],
    sampler linearSampler [[sampler(0)]],
    constant FogCombineUniforms& u [[buffer(5)]])
{
    const float2 uv = clamp(in.uv, float2(0.0), float2(1.0));
    const float cameraNear = u.cameraParams.x;
    const float cameraFar = u.cameraParams.y;

    const float depth = getLinearDepth(depthTexture.sample(linearSampler, uv), cameraNear, cameraFar);

    // The four nearest texel centres of the low-resolution fog texture.
    const float2 texel = uv * u.textureSize.xy - 0.5;
    const float2 base = (floor(texel) + 0.5) * u.textureSize.zw;
    const float2 f = fract(texel);

    float2 uvs[4];
    uvs[0] = base;
    uvs[1] = base + float2(u.textureSize.z, 0.0);
    uvs[2] = base + float2(0.0, u.textureSize.w);
    uvs[3] = base + u.textureSize.zw;

    const float4 bilinear = float4(
        (1.0 - f.x) * (1.0 - f.y),
        f.x * (1.0 - f.y),
        (1.0 - f.x) * f.y,
        f.x * f.y);

    // Depth-aware upsample: weight each low-resolution sample by how closely its depth matches
    // this pixel's, so fog does not leak across geometry edges.
    float4 sum = float4(0.0);
    float sumWeight = 0.0;
    for (int i = 0; i < 4; ++i) {
        const float sampleDepth = getLinearDepth(
            depthTexture.sample(linearSampler, uvs[i]), cameraNear, cameraFar);
        const float w = bilinear[i] / (1.0 + 16.0 * abs(sampleDepth - depth) / max(depth, 0.001));
        sum += fogTexture.sample(linearSampler, uvs[i]) * w;
        sumWeight += w;
    }

    // rgb = in-scattered light, a = transmittance. The blend state resolves this to
    // `scene * transmittance + inscatter`.
    return sum / max(sumWeight, 0.0001);
}
)";

        std::shared_ptr<Shader> createFogShader(MetalGraphicsDevice* device,
            const std::string& name, const char* vertexEntry, const char* fragmentEntry,
            const char* source)
        {
            ShaderDefinition definition;
            definition.name = name;
            definition.vshader = vertexEntry;
            definition.fshader = fragmentEntry;
            return createShader(device, definition, source);
        }
    }

    MetalVolumetricFogPass::MetalVolumetricFogPass(MetalGraphicsDevice* device,
        MetalComposePass* composePass)
        : _device(device), _composePass(composePass)
    {
    }

    MetalVolumetricFogPass::~MetalVolumetricFogPass()
    {
        if (_depthStencilState) {
            _depthStencilState->release();
            _depthStencilState = nullptr;
        }
    }

    void MetalVolumetricFogPass::ensureResources()
    {
        _composePass->ensureResources();

        if (_fogShader && _combineShader && _composePass->vertexBuffer() && _composePass->vertexFormat() &&
            _blendState && _combineBlendState && _depthState && _depthStencilState) {
            return;
        }

        if (!_fogShader) {
            _fogShader = createFogShader(_device, "VolumetricFogPass", "fogVertex", "fogFragment",
                VOLUMETRIC_FOG_SOURCE);
        }
        if (!_combineShader) {
            _combineShader = createFogShader(_device, "VolumetricFogCombinePass",
                "fogCombineVertex", "fogCombineFragment", VOLUMETRIC_FOG_COMBINE_SOURCE);
        }

        if (!_blendState) {
            // The ray-march pass overwrites its target.
            _blendState = std::make_shared<BlendState>();
        }
        if (!_combineBlendState) {
            // scene * transmittance + inscatter
            _combineBlendState = std::make_shared<BlendState>();
            _combineBlendState->setEnabled(true);
            _combineBlendState->setColorOp(BLENDEQUATION_ADD);
            _combineBlendState->setColorSrcFactor(BLENDMODE_ONE);
            _combineBlendState->setColorDstFactor(BLENDMODE_SRC_ALPHA);
            _combineBlendState->setAlphaOp(BLENDEQUATION_ADD);
            _combineBlendState->setAlphaSrcFactor(BLENDMODE_ZERO);
            _combineBlendState->setAlphaDstFactor(BLENDMODE_ONE);
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

    void MetalVolumetricFogPass::execute(MTL::RenderCommandEncoder* encoder,
        const VolumetricFogPassParams& params,
        MetalRenderPipeline* pipeline, const std::shared_ptr<RenderTarget>& renderTarget,
        const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats,
        MTL::SamplerState* defaultSampler, MTL::DepthStencilState* defaultDepthStencilState)
    {
        if (!encoder || !params.depthTexture) {
            return;
        }

        ensureResources();
        if (!_fogShader || !_composePass->vertexBuffer() || !_composePass->vertexFormat() ||
            !_blendState || !_depthState) {
            spdlog::warn("[executeVolumetricFogPass] missing resources");
            return;
        }

        Primitive primitive;
        primitive.type = PRIMITIVE_TRIANGLES;
        primitive.base = 0;
        primitive.count = 3;
        primitive.indexed = false;

        auto pipelineState = pipeline->get(primitive, _composePass->vertexFormat(), nullptr, -1,
            _fogShader, renderTarget, bindGroupFormats, _blendState, _depthState,
            CullMode::CULLFACE_NONE, false, nullptr, nullptr);
        if (!pipelineState) {
            spdlog::warn("[executeVolumetricFogPass] failed to get pipeline state");
            return;
        }

        auto* vb = dynamic_cast<MetalVertexBuffer*>(_composePass->vertexBuffer().get());
        if (!vb || !vb->raw()) {
            spdlog::warn("[executeVolumetricFogPass] missing vertex buffer");
            return;
        }

        encoder->setRenderPipelineState(pipelineState);
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setDepthStencilState(_depthStencilState ? _depthStencilState : defaultDepthStencilState);
        encoder->setVertexBuffer(vb->raw(), 0, 0);

        auto* depthHw = dynamic_cast<gpu::MetalTexture*>(params.depthTexture->impl());
        encoder->setFragmentTexture(depthHw ? depthHw->raw() : nullptr, 0);

        // The shadow texture slot must always be bound: Metal requires every declared texture
        // argument to have a resource, even on the branch that does not sample it. Fall back to
        // the depth texture (also a depth2d) when the light has no shadow map.
        auto* shadowHw = params.shadowTexture
            ? dynamic_cast<gpu::MetalTexture*>(params.shadowTexture->impl())
            : depthHw;
        encoder->setFragmentTexture(shadowHw ? shadowHw->raw() : nullptr, 1);

        if (defaultSampler) {
            encoder->setFragmentSamplerState(defaultSampler, 0);
        }

        // IMPORTANT: must match the shader's FogUniforms layout exactly. Every member is a
        // float4/float4x4 so the natural 16-byte alignment needs no manual padding.
        struct alignas(16) FogUniforms
        {
            float invView[16];
            float shadowMatrixPalette[4][16];
            float cameraPosition[4];
            float cameraForward[4];
            float projScale[4];
            float tint[4];
            float lightColor[4];
            float lightDirection[4];
            float ambient[4];
            float fogParams[4];
            float scatterParams[4];
            float shadowCascadeDistances[4];
            float shadowParams[4];
            float cameraParams[4];
        } uniforms{};

        // Matrix4::getElement takes (col, row); Metal float4x4 is column-major, so element
        // [col * 4 + row] matches directly.
        const auto packMatrix = [](const Matrix4& m, float* dest) {
            for (int col = 0; col < 4; ++col) {
                for (int row = 0; row < 4; ++row) {
                    dest[col * 4 + row] = m.getElement(col, row);
                }
            }
        };

        packMatrix(params.invView, uniforms.invView);
        for (int i = 0; i < 4; ++i) {
            packMatrix(params.shadowMatrixPalette[i], uniforms.shadowMatrixPalette[i]);
        }

        for (int i = 0; i < 3; ++i) {
            uniforms.cameraPosition[i] = params.cameraPosition[i];
            uniforms.cameraForward[i] = params.cameraForward[i];
            uniforms.tint[i] = params.tint[i];
            uniforms.lightColor[i] = params.lightColor[i];
            uniforms.lightDirection[i] = params.lightDirection[i];
            uniforms.ambient[i] = params.ambient[i];
        }

        uniforms.projScale[0] = params.projScaleX;
        uniforms.projScale[1] = params.projScaleY;

        uniforms.fogParams[0] = params.density;
        uniforms.fogParams[1] = params.heightBase;
        uniforms.fogParams[2] = params.heightFalloff;
        uniforms.fogParams[3] = params.maxDistance;

        uniforms.scatterParams[0] = params.anisotropy;
        uniforms.scatterParams[1] = params.stepCount;
        uniforms.scatterParams[2] = params.noiseOffset;
        uniforms.scatterParams[3] = params.shadowIntensity;

        for (int i = 0; i < 4; ++i) {
            uniforms.shadowCascadeDistances[i] = params.shadowCascadeDistances[i];
        }
        uniforms.shadowParams[0] = params.shadowCascadeCount;
        uniforms.shadowParams[1] = params.shadowBias;
        uniforms.shadowParams[2] = params.shadowTexture ? 1.0f : 0.0f;
        uniforms.shadowParams[3] = params.shadowDistance;

        uniforms.cameraParams[0] = params.cameraNear;
        uniforms.cameraParams[1] = params.cameraFar;
        uniforms.cameraParams[2] = params.extinction;

        encoder->setFragmentBytes(&uniforms, sizeof(FogUniforms), 5);

        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, static_cast<NS::UInteger>(0),
            static_cast<NS::UInteger>(3));
        _device->recordDrawCall();
    }

    void MetalVolumetricFogPass::executeCombine(MTL::RenderCommandEncoder* encoder,
        const VolumetricFogCombineParams& params,
        MetalRenderPipeline* pipeline, const std::shared_ptr<RenderTarget>& renderTarget,
        const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats,
        MTL::SamplerState* defaultSampler, MTL::DepthStencilState* defaultDepthStencilState)
    {
        if (!encoder || !params.depthTexture || !params.fogTexture) {
            return;
        }

        ensureResources();
        if (!_combineShader || !_composePass->vertexBuffer() || !_composePass->vertexFormat() ||
            !_combineBlendState || !_depthState) {
            spdlog::warn("[executeVolumetricFogCombinePass] missing resources");
            return;
        }

        Primitive primitive;
        primitive.type = PRIMITIVE_TRIANGLES;
        primitive.base = 0;
        primitive.count = 3;
        primitive.indexed = false;

        auto pipelineState = pipeline->get(primitive, _composePass->vertexFormat(), nullptr, -1,
            _combineShader, renderTarget, bindGroupFormats, _combineBlendState, _depthState,
            CullMode::CULLFACE_NONE, false, nullptr, nullptr);
        if (!pipelineState) {
            spdlog::warn("[executeVolumetricFogCombinePass] failed to get pipeline state");
            return;
        }

        auto* vb = dynamic_cast<MetalVertexBuffer*>(_composePass->vertexBuffer().get());
        if (!vb || !vb->raw()) {
            spdlog::warn("[executeVolumetricFogCombinePass] missing vertex buffer");
            return;
        }

        encoder->setRenderPipelineState(pipelineState);
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setDepthStencilState(_depthStencilState ? _depthStencilState : defaultDepthStencilState);
        encoder->setVertexBuffer(vb->raw(), 0, 0);

        auto* depthHw = dynamic_cast<gpu::MetalTexture*>(params.depthTexture->impl());
        encoder->setFragmentTexture(depthHw ? depthHw->raw() : nullptr, 0);
        auto* fogHw = dynamic_cast<gpu::MetalTexture*>(params.fogTexture->impl());
        encoder->setFragmentTexture(fogHw ? fogHw->raw() : nullptr, 1);
        if (defaultSampler) {
            encoder->setFragmentSamplerState(defaultSampler, 0);
        }

        struct alignas(16) FogCombineUniforms
        {
            float textureSize[4];
            float cameraParams[4];
        } uniforms{};

        const float width = std::max(params.fogTextureWidth, 1.0f);
        const float height = std::max(params.fogTextureHeight, 1.0f);
        uniforms.textureSize[0] = width;
        uniforms.textureSize[1] = height;
        uniforms.textureSize[2] = 1.0f / width;
        uniforms.textureSize[3] = 1.0f / height;
        uniforms.cameraParams[0] = params.cameraNear;
        uniforms.cameraParams[1] = params.cameraFar;

        encoder->setFragmentBytes(&uniforms, sizeof(FogCombineUniforms), 5);

        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, static_cast<NS::UInteger>(0),
            static_cast<NS::UInteger>(3));
        _device->recordDrawCall();
    }
}
