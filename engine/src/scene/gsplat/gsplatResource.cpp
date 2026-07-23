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
        // GSPLAT_SHADER_SOURCE is embedded from shaders/metal/embedded/gsplat-render.metal at build
        // time (see tools/embed_msl.cmake).
#ifdef VISUTWIN_HAS_METAL
#include "embedded_shaders/gsplat-render.metal.inc"
#else
        constexpr const char* GSPLAT_SHADER_SOURCE = "";
#endif
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

        // ── SH coefficient buffer (vertex slot 12) ───────────────────────
        // 45 floats/splat (coefficient-major interleaved) when shBands > 0; a
        // 1-float dummy otherwise so slot 12 is always bound (the shader only
        // reads it under the runtime shBands branch).
        {
            const auto& sh = _data->shCoeffs();
            const bool hasSh = _data->shBands() > 0 && !sh.empty();
            std::vector<uint8_t> shBytes(hasSh ? sh.size() * sizeof(float) : sizeof(float), 0);
            if (hasSh) {
                std::memcpy(shBytes.data(), sh.data(), shBytes.size());
            }
            const int shFloats = hasSh ? static_cast<int>(sh.size()) : 1;
            auto shFormat = std::make_shared<VertexFormat>(static_cast<int>(sizeof(float)), true, false);
            VertexBufferOptions shOptions;
            shOptions.data = std::move(shBytes);
            _shBuffer = device->createVertexBuffer(shFormat, shFloats, shOptions);
        }

        // ── Quad mesh: 4 dummy vertices, one triangle-strip quad per instance ──
        // The vertex shader is [[vertex_id]]-driven; the buffer only satisfies the
        // renderer's non-null vertex buffer requirement.
        auto quadFormat = std::make_shared<VertexFormat>(
            14 * static_cast<int>(sizeof(float)), VertexFormat::standardElements(), true, false);
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
