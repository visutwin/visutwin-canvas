// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// SSAO (Screen-Space Ambient Occlusion) pass implementation.
// Shader ported from upstream scene/shader-lib/glsl/chunks/render-pass/frag/ssao.js
//
#include "metalSsaoPass.h"

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
        // SSAO (GLSL to Metal).
        // Based on 'Scalable Ambient Obscurance' by Morgan McGuire, adapted by Naughty Dog.
        constexpr const char* SSAO_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

struct ComposeVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv0 [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float2 uv1 [[attribute(4)]];
};

struct SsaoVarying {
    float4 position [[position]];
    float2 uv;
};

struct SsaoUniforms {
    float aspect;
    float2 invResolution;
    float2 sampleCount; // x=count, y=1/count
    float spiralTurns;
    float2 angleIncCosSin;
    float maxLevel;
    float invRadiusSquared;
    float minHorizonAngleSineSquared;
    float bias;
    float peak2;
    float intensity;
    float power;
    float projectionScaleRadius;
    float randomize;
    float cameraNear;
    float cameraFar;
};

vertex SsaoVarying ssaoVertex(ComposeVertexIn in [[stage_in]])
{
    SsaoVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

static inline float getLinearDepth(float rawDepth, float cameraNear, float cameraFar)
{
    // Standard depth [0,1]: near=0, far=1 (vertex shader maps via clip.z = 0.5*(clip.z + clip.w)).
    // Returns positive linear view-space distance from camera.
    return (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
}

static constant float kLog2LodRate = 3.0;

// Random number between 0 and 1 using interleaved gradient noise
static inline float random(float2 fragCoord)
{
    const float3 m = float3(0.06711056, 0.00583715, 52.9829189);
    return fract(m.z * fract(dot(fragCoord, m.xy)));
}

static inline float3 computeViewSpacePositionFromDepth(float2 uv, float linearDepth, float aspect)
{
    return float3((0.5 - uv) * float2(aspect, 1.0) * linearDepth, linearDepth);
}

static inline float3 faceNormal(float3 dpdx, float3 dpdy)
{
    return normalize(cross(dpdx, dpdy));
}

// Compute normals directly from the depth texture (full resolution normals)
static inline float3 computeViewSpaceNormal(float3 position, float2 uv, float2 invResolution,
    float aspect, depth2d<float> depthTexture, sampler linearSampler, float cameraNear, float cameraFar)
{
    float2 uvdx = uv + float2(invResolution.x, 0.0);
    float2 uvdy = uv + float2(0.0, invResolution.y);
    float depthDx = depthTexture.sample(linearSampler, uvdx);
    float depthDy = depthTexture.sample(linearSampler, uvdy);
    float3 px = computeViewSpacePositionFromDepth(uvdx, getLinearDepth(depthDx, cameraNear, cameraFar), aspect);
    float3 py = computeViewSpacePositionFromDepth(uvdy, getLinearDepth(depthDy, cameraNear, cameraFar), aspect);
    float3 dpdx = px - position;
    float3 dpdy = py - position;
    return faceNormal(dpdx, dpdy);
}

// Spiral tap position (fast path)
static inline float2 startPosition(float noise)
{
    float angle = ((2.0 * M_PI_F) * 2.4) * noise;
    return float2(cos(angle), sin(angle));
}

static inline float3 tapLocationFast(float i, float2 p, float noise, float invSampleCount)
{
    float radius = (i + noise + 0.5) * invSampleCount;
    return float3(p, radius * radius);
}

static inline float2x2 tapAngleStep(float2 angleIncCosSin)
{
    return float2x2(angleIncCosSin.x, angleIncCosSin.y, -angleIncCosSin.y, angleIncCosSin.x);
}

static inline void computeAmbientOcclusionSAO(
    thread float& occlusion, float i, float ssDiskRadius,
    float2 uv, float3 origin, float3 normal,
    float2 tapPosition, float noise, float invSampleCount,
    float2 invResolution, float invRadiusSquared, float minHorizonAngleSineSquared,
    float bias, float peak2, float aspect,
    depth2d<float> depthTexture, sampler linearSampler, float cameraNear, float cameraFar)
{
    float3 tap = tapLocationFast(i, tapPosition, noise, invSampleCount);

    float ssRadius = max(1.0, tap.z * ssDiskRadius); // at least 1 pixel screen-space radius

    float2 uvSamplePos = uv + float2(ssRadius * tap.xy) * invResolution;

    float occlusionDepth = getLinearDepth(depthTexture.sample(linearSampler, uvSamplePos), cameraNear, cameraFar);
    float3 p = computeViewSpacePositionFromDepth(uvSamplePos, occlusionDepth, aspect);

    // now we have the sample, compute AO
    float3 v = p - origin;        // sample vector
    float vv = dot(v, v);       // squared distance
    float vn = dot(v, normal);  // distance * cos(v, normal)

    // discard samples that are outside of the radius
    float w = max(0.0, 1.0 - vv * invRadiusSquared);
    w = w * w;

    // discard samples that are too close to the horizon
    w *= step(vv * minHorizonAngleSineSquared, vn * vn);

    occlusion += w * max(0.0, vn + origin.z * bias) / (vv + peak2);
}

static inline float scalableAmbientObscurance(
    float2 uv, float3 origin, float3 normal, float2 fragCoord,
    float2 sampleCount, float2 angleIncCosSin, float projectionScaleRadius,
    float2 invResolution, float invRadiusSquared, float minHorizonAngleSineSquared,
    float bias, float peak2, float aspect, float randomizeValue,
    depth2d<float> depthTexture, sampler linearSampler, float cameraNear, float cameraFar)
{
    float noise = random(fragCoord) + randomizeValue;
    float2 tapPos = startPosition(noise);
    float2x2 angleStep = tapAngleStep(angleIncCosSin);

    // Choose the screen-space sample radius proportional to the projected area of the sphere
    // DEVIATION: upstream uses -(projInfo.z / position.z) with negative Z (OpenGL -Z convention).
    // Our view-space Z is positive (linearDepth), so we use positive division directly.
    float ssDiskRadius = projectionScaleRadius / origin.z;

    float occlusion = 0.0;
    for (float i = 0.0; i < sampleCount.x; i += 1.0) {
        computeAmbientOcclusionSAO(occlusion, i, ssDiskRadius, uv, origin, normal, tapPos, noise,
            sampleCount.y, invResolution, invRadiusSquared, minHorizonAngleSineSquared,
            bias, peak2, aspect, depthTexture, linearSampler, cameraNear, cameraFar);
        tapPos = angleStep * tapPos;
    }
    return occlusion;
}

fragment float4 ssaoFragment(
    SsaoVarying in [[stage_in]],
    depth2d<float> depthTexture [[texture(0)]],
    sampler linearSampler [[sampler(0)]],
    constant SsaoUniforms& uniforms [[buffer(5)]])
{
    const float2 uv = clamp(in.uv, float2(0.0), float2(1.0));

    float rawDepth = depthTexture.sample(linearSampler, uv);
    float depth = getLinearDepth(rawDepth, uniforms.cameraNear, uniforms.cameraFar);
    float3 origin = computeViewSpacePositionFromDepth(uv, depth, uniforms.aspect);
    // DEVIATION: upstream reconstructs positions with negative Z (depth = -getLinearScreenDepth),
    // so cross(dpdx, dpdy) naturally yields normals pointing towards the camera (-Z).
    // Our Metal path uses positive depth (distance from camera), so the cross product
    // produces normals pointing away (+Z).  Negate to match upstream convention.
    float3 normal = -computeViewSpaceNormal(origin, uv, uniforms.invResolution, uniforms.aspect,
        depthTexture, linearSampler, uniforms.cameraNear, uniforms.cameraFar);

    float occlusion = 0.0;
    if (uniforms.intensity > 0.0) {
        occlusion = scalableAmbientObscurance(uv, origin, normal, in.position.xy,
            uniforms.sampleCount, uniforms.angleIncCosSin, uniforms.projectionScaleRadius,
            uniforms.invResolution, uniforms.invRadiusSquared, uniforms.minHorizonAngleSineSquared,
            uniforms.bias, uniforms.peak2, uniforms.aspect, uniforms.randomize,
            depthTexture, linearSampler, uniforms.cameraNear, uniforms.cameraFar);
    }

    // occlusion to visibility
    float ao = max(0.0, 1.0 - occlusion * uniforms.intensity);
    ao = pow(ao, uniforms.power);

    return float4(ao, ao, ao, 1.0);
}
)";
    }

    MetalSsaoPass::MetalSsaoPass(MetalGraphicsDevice* device, MetalComposePass* composePass)
        : _device(device), _composePass(composePass)
    {
    }

    MetalSsaoPass::~MetalSsaoPass()
    {
        if (_depthStencilState) {
            _depthStencilState->release();
            _depthStencilState = nullptr;
        }
    }

    void MetalSsaoPass::ensureResources()
    {
        // Ensure the compose pass's shared vertex buffer/format are created first
        _composePass->ensureResources();

        if (_shader && _composePass->vertexBuffer() && _composePass->vertexFormat() &&
            _blendState && _depthState && _depthStencilState) {
            return;
        }

        if (!_shader) {
            ShaderDefinition definition;
            definition.name = "SsaoPass";
            definition.vshader = "ssaoVertex";
            definition.fshader = "ssaoFragment";
            _shader = createShader(_device, definition, SSAO_SOURCE);
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

    void MetalSsaoPass::execute(MTL::RenderCommandEncoder* encoder,
        const SsaoPassParams& params,
        MetalRenderPipeline* pipeline, const std::shared_ptr<RenderTarget>& renderTarget,
        const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats,
        MTL::SamplerState* defaultSampler, MTL::DepthStencilState* defaultDepthStencilState)
    {
        if (!encoder || !params.depthTexture) {
            return;
        }

        ensureResources();
        if (!_shader || !_composePass->vertexBuffer() || !_composePass->vertexFormat() || !_blendState || !_depthState) {
            spdlog::warn("[executeSsaoPass] missing SSAO resources");
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
            spdlog::warn("[executeSsaoPass] failed to get pipeline state");
            return;
        }

        auto* vb = dynamic_cast<MetalVertexBuffer*>(_composePass->vertexBuffer().get());
        if (!vb || !vb->raw()) {
            spdlog::warn("[executeSsaoPass] missing vertex buffer");
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

        // IMPORTANT: This struct must match the Metal shader's SsaoUniforms layout exactly.
        // Metal float2 has 8-byte alignment, so padding is needed after scalar floats that
        // precede float2 members (aspect before invResolution, spiralTurns before angleIncCosSin).
        struct alignas(16) SsaoUniforms
        {
            float aspect;                       // offset 0
            float _pad0;                        // offset 4  (align invResolution to 8-byte boundary)
            float invResolution[2];             // offset 8  (matches Metal float2)
            float sampleCount[2];               // offset 16 (matches Metal float2)
            float spiralTurns;                  // offset 24
            float _pad1;                        // offset 28 (align angleIncCosSin to 8-byte boundary)
            float angleIncCosSin[2];            // offset 32 (matches Metal float2)
            float maxLevel;                     // offset 40
            float invRadiusSquared;             // offset 44
            float minHorizonAngleSineSquared;   // offset 48
            float bias;                         // offset 52
            float peak2;                        // offset 56
            float intensity;                    // offset 60
            float power;                        // offset 64
            float projectionScaleRadius;        // offset 68
            float randomize;                    // offset 72
            float cameraNear;                   // offset 76
            float cameraFar;                    // offset 80
        } uniforms{};

        uniforms.aspect = params.aspect;
        uniforms._pad0 = 0.0f;
        uniforms.invResolution[0] = params.invResolutionX;
        uniforms.invResolution[1] = params.invResolutionY;
        uniforms.sampleCount[0] = static_cast<float>(params.sampleCount);
        uniforms.sampleCount[1] = 1.0f / static_cast<float>(params.sampleCount);
        uniforms.spiralTurns = params.spiralTurns;
        uniforms._pad1 = 0.0f;
        uniforms.angleIncCosSin[0] = params.angleIncCos;
        uniforms.angleIncCosSin[1] = params.angleIncSin;
        uniforms.maxLevel = 0.0f;
        uniforms.invRadiusSquared = params.invRadiusSquared;
        uniforms.minHorizonAngleSineSquared = params.minHorizonAngleSineSquared;
        uniforms.bias = params.bias;
        uniforms.peak2 = params.peak2;
        uniforms.intensity = params.intensity;
        uniforms.power = params.power;
        uniforms.projectionScaleRadius = params.projectionScaleRadius;
        uniforms.randomize = params.randomize;
        uniforms.cameraNear = params.cameraNear;
        uniforms.cameraFar = params.cameraFar;
        encoder->setFragmentBytes(&uniforms, sizeof(SsaoUniforms), 5);

        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, static_cast<NS::UInteger>(0),
            static_cast<NS::UInteger>(3));
        _device->recordDrawCall();
    }
}
