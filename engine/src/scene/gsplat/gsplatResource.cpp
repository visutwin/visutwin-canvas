// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "gsplatResource.h"

#include <cstring>

#include <spdlog/spdlog.h>

#include "gsplatInstance.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/materials/material.h"

namespace visutwin::canvas
{
    namespace
    {
        // Standalone Metal shader for Gaussian splats. Line-faithful port of the
        // EWA screen-space covariance projection in upstream gsplatCorner.js and
        // the normalized-exponential falloff in the gsplat fragment chunk.
        // DEVIATION: self-contained source (not composed from the chunk registry) —
        // the splat pipeline shares no code with the forward PBR mega-chunks.
        constexpr const char* GSPLAT_SHADER_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

struct GpuSplat {
    packed_float3 center;
    uint color;             // RGBA8: SH0 color + opacity
    packed_float3 covA;     // Sigma00, Sigma01, Sigma02
    packed_float3 covB;     // Sigma11, Sigma12, Sigma22
};

struct GSplatParams {
    float4x4 modelView;
    float4x4 projection;    // GL-style clip (z in [-w,w]); remapped below
    float4 viewport;        // width, height, 1/width, 1/height
    uint splatCount;
    uint pad0; uint pad1; uint pad2;
};

struct GSplatVaryings {
    float4 position [[position]];
    float2 uv;
    half4 color;
};

vertex GSplatVaryings gsplatVS(uint vid [[vertex_id]],
                               uint iid [[instance_id]],
                               constant GpuSplat* splats [[buffer(7)]],
                               constant uint* order [[buffer(8)]],
                               constant GSplatParams& params [[buffer(11)]])
{
    GSplatVaryings out;
    out.position = float4(0.0, 0.0, 2.0, 1.0);  // default: clipped
    out.uv = float2(0.0);
    out.color = half4(0.0h);

    const float2 cornerUV[4] = { float2(-1.0, -1.0), float2(1.0, -1.0),
                                 float2(-1.0, 1.0), float2(1.0, 1.0) };

    const uint splatIndex = order[iid];
    if (splatIndex >= params.splatCount) {
        return out;
    }
    const GpuSplat s = splats[splatIndex];

    const float4 view = params.modelView * float4(float3(s.center), 1.0);
    float4 clip = params.projection * view;
    if (clip.w <= 0.0) {
        return out;
    }

    // 3D covariance in splat model space.
    const float3 covA = float3(s.covA);
    const float3 covB = float3(s.covB);
    const float3x3 Vrk = float3x3(float3(covA.x, covA.y, covA.z),
                                  float3(covA.y, covB.x, covB.y),
                                  float3(covA.z, covB.y, covB.z));

    // Perspective Jacobian at the splat center (upstream gsplatCorner.js).
    const float focal = params.viewport.x * params.projection[0][0];
    const float3 v = view.xyz;
    const float J1 = focal / v.z;
    const float2 J2 = -J1 / v.z * v.xy;
    const float3x3 J = float3x3(float3(J1, 0.0, J2.x),
                                float3(0.0, J1, J2.y),
                                float3(0.0, 0.0, 0.0));

    const float3x3 W = transpose(float3x3(params.modelView[0].xyz,
                                          params.modelView[1].xyz,
                                          params.modelView[2].xyz));
    const float3x3 T = W * J;
    const float3x3 cov = transpose(T) * Vrk * T;

    // 2D covariance eigen decomposition (+0.3 px low-pass dilation).
    const float diagonal1 = cov[0][0] + 0.3;
    const float offDiagonal = cov[0][1];
    const float diagonal2 = cov[1][1] + 0.3;
    const float mid = 0.5 * (diagonal1 + diagonal2);
    const float radius = length(float2((diagonal1 - diagonal2) * 0.5, offDiagonal));
    const float lambda1 = mid + radius;
    const float lambda2 = max(mid - radius, 0.1);

    const float vmin = min(1024.0, min(params.viewport.x, params.viewport.y));
    const float l1 = 2.0 * min(sqrt(2.0 * lambda1), vmin);
    const float l2 = 2.0 * min(sqrt(2.0 * lambda2), vmin);

    // Cull sub-pixel gaussians.
    if (max(l1, l2) < 0.5) {
        return out;
    }

    // Cull against the frustum x/y planes.
    const float2 c = clip.w * params.viewport.zw;
    if (any(abs(clip.xy) - float2(max(l1, l2)) * c > clip.ww)) {
        return out;
    }

    const float2 diagonalVector = normalize(float2(offDiagonal, lambda1 - diagonal1));
    const float2 v1 = l1 * diagonalVector;
    const float2 v2 = l2 * float2(diagonalVector.y, -diagonalVector.x);

    const float2 uv = cornerUV[vid];
    clip.xy += (uv.x * v1 + uv.y * v2) * c;

    // DEVIATION: OpenGL NDC z range is [-1,1]; Metal requires [0,1].
    clip.z = 0.5 * (clip.z + clip.w);

    const half4 color = unpack_unorm4x8_to_half(s.color);
    out.position = clip;
    out.uv = uv;
    // sRGB-ish splat color → linear (the HDR pipeline tonemaps/encodes on output).
    out.color = half4(pow(color.rgb, half3(2.2h)), color.a);
    return out;
}

fragment half4 gsplatFS(GSplatVaryings in [[stage_in]])
{
    const float A = dot(in.uv, in.uv);
    if (A > 1.0) {
        discard_fragment();
    }
    // Normalized exponential falloff (upstream normExp): 1 at center, 0 at edge.
    const float EXP4 = 0.018315638889f;
    const float alpha = ((exp(-4.0f * A) - EXP4) / (1.0f - EXP4)) * float(in.color.a);
    return half4(half3(in.color.rgb) * half(alpha), half(alpha));
}
)";
    }

    GSplatResource::GSplatResource(std::unique_ptr<GSplatData> data,
        const std::shared_ptr<GraphicsDevice>& device)
        : _device(device), _data(std::move(data))
    {
        // ── Splat storage buffer (vertex slot 7) ─────────────────────────
        const auto& splats = _data->splats();
        std::vector<uint8_t> splatBytes(splats.size() * sizeof(GpuSplat));
        std::memcpy(splatBytes.data(), splats.data(), splatBytes.size());
        auto splatFormat = std::make_shared<VertexFormat>(static_cast<int>(sizeof(GpuSplat)), true, false);
        VertexBufferOptions splatOptions;
        splatOptions.data = std::move(splatBytes);
        _splatBuffer = device->createVertexBuffer(splatFormat, _data->numSplats(), splatOptions);

        // ── Quad mesh: 4 dummy vertices, one triangle-strip quad per instance ──
        // The vertex shader is [[vertex_id]]-driven; the buffer only satisfies the
        // renderer's non-null vertex buffer requirement.
        auto quadFormat = std::make_shared<VertexFormat>(14 * static_cast<int>(sizeof(float)), true, false);
        VertexBufferOptions quadOptions;
        quadOptions.data.assign(4 * 14 * sizeof(float), 0);
        auto quadBuffer = device->createVertexBuffer(quadFormat, 4, quadOptions);

        _quadMesh = std::make_shared<Mesh>();
        _quadMesh->setVertexBuffer(quadBuffer);
        Primitive primitive;
        primitive.type = PRIMITIVE_TRISTRIP;
        primitive.base = 0;
        primitive.count = 4;
        primitive.indexed = false;
        _quadMesh->setPrimitive(primitive, 0);
        _quadMesh->setAabb(_data->aabb());

        // ── Splat shader + material ──────────────────────────────────────
        ShaderDefinition definition;
        definition.name = "gsplat";
        definition.vshader = "gsplatVS";
        definition.fshader = "gsplatFS";
        _shader = createShader(device.get(), definition, GSPLAT_SHADER_SOURCE);

        _material = std::make_shared<Material>();
        _material->setName("gsplat");
        _material->setTransparent(true);
        _material->setShaderOverride(_shader);
        // Screen-space quads have no meaningful winding — never cull.
        _material->setCullMode(CullMode::CULLFACE_NONE);

        // Premultiplied alpha over (fragment outputs rgb * alpha).
        auto blendState = std::make_shared<BlendState>();
        blendState->setEnabled(true);
        blendState->setColorOp(BLENDEQUATION_ADD);
        blendState->setColorSrcFactor(BLENDMODE_ONE);
        blendState->setColorDstFactor(BLENDMODE_ONE_MINUS_SRC_ALPHA);
        blendState->setAlphaOp(BLENDEQUATION_ADD);
        blendState->setAlphaSrcFactor(BLENDMODE_ONE);
        blendState->setAlphaDstFactor(BLENDMODE_ONE_MINUS_SRC_ALPHA);
        _material->setBlendState(blendState);

        // Depth test against opaques, no depth write (internally sorted).
        auto depthState = std::make_shared<DepthState>();
        depthState->setDepthTest(true);
        depthState->setDepthWrite(false);
        _material->setDepthState(depthState);
    }

    std::shared_ptr<GSplatResource> GSplatResource::loadPly(const std::string& path,
        const std::shared_ptr<GraphicsDevice>& device)
    {
        auto data = GSplatData::loadPly(path);
        if (!data || !device) {
            return nullptr;
        }
        return std::make_shared<GSplatResource>(std::move(data), device);
    }

    std::unique_ptr<MeshInstance> GSplatResource::createMeshInstance(GraphNode* node)
    {
        auto meshInstance = std::make_unique<MeshInstance>(_quadMesh, _material, node);
        meshInstance->setCastShadow(false);
        meshInstance->setGSplatInstance(
            std::make_shared<GSplatInstance>(shared_from_this()));
        return meshInstance;
    }
}
