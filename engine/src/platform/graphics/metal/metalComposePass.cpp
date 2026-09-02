// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Compose post-processing pass implementation.
// Extracted from MetalGraphicsDevice.
//
#include "metalComposePass.h"

#include <cstring>
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
    }

    MetalComposePass::MetalComposePass(MetalGraphicsDevice* device)
        : _device(device)
    {
    }

    MetalComposePass::~MetalComposePass()
    {
        if (_depthStencilState) {
            _depthStencilState->release();
            _depthStencilState = nullptr;
        }
    }

    void MetalComposePass::ensureResources()
    {
        if (_vertexBuffer && _vertexFormat && _blendState &&
            _depthState && _depthStencilState) {
            return;
        }

        if (!_vertexFormat) {
            _vertexFormat = std::make_shared<VertexFormat>(
                static_cast<int>(14 * sizeof(float)), VertexFormat::standardElements(), true, false);
        }

        if (!_vertexBuffer && _vertexFormat) {
            // DEVIATION: Metal/WebGPU texture UV origin is top-left (V=0 at top).
            // Upstream handles this via getImageEffectUV() Y-flip in shader.
            // We flip UV.y here: clip Y=-1 (bottom) -> UV.y=1 (bottom of texture),
            // clip Y=+1 (top) -> UV.y=0 (top of texture).
            constexpr float vertexData[3 * 14] = {
                // pos.xyz         normal.xyz      uv0.xy    tangent.xyzw      uv1.xy
                -1.0f, -1.0f, 0.0f, 0, 0, 1,       0.0f, 1.0f,   1, 0, 0, 1,   0.0f, 1.0f,
                 3.0f, -1.0f, 0.0f, 0, 0, 1,       2.0f, 1.0f,   1, 0, 0, 1,   0.0f, 1.0f,
                -1.0f,  3.0f, 0.0f, 0, 0, 1,       0.0f,-1.0f,   1, 0, 0, 1,   0.0f,-1.0f
            };
            VertexBufferOptions options;
            options.usage = BUFFER_STATIC;
            options.data.resize(sizeof(vertexData));
            std::memcpy(options.data.data(), vertexData, sizeof(vertexData));
            _vertexBuffer = _device->createVertexBuffer(_vertexFormat, 3, options);
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

}
