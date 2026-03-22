// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Shared Metal utility functions and enum converters.
// Extracted from MetalGraphicsDevice to enable reuse across Metal backend classes.
//
#pragma once

#include <simd/simd.h>
#include <Metal/Metal.hpp>

#include "core/math/matrix4.h"
#include "platform/graphics/constants.h"
#include "platform/graphics/renderTarget.h"
#include "scene/mesh.h"

namespace visutwin::canvas::metal
{
    /// Convert a column-major Matrix4 to a SIMD float4x4.
    inline simd::float4x4 toSimdMatrix(const Matrix4& matrix)
    {
        simd::float4x4 out{};
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                out.columns[col][row] = matrix.getElement(col, row);
            }
        }
        return out;
    }

    /// Map engine PrimitiveType to Metal PrimitiveType.
    inline MTL::PrimitiveType toMetalPrimitiveType(const PrimitiveType primitiveType)
    {
        switch (primitiveType) {
        case PRIMITIVE_POINTS:
            return MTL::PrimitiveTypePoint;
        case PRIMITIVE_LINES:
            return MTL::PrimitiveTypeLine;
        case PRIMITIVE_LINESTRIP:
        case PRIMITIVE_LINELOOP:
            return MTL::PrimitiveTypeLineStrip;
        case PRIMITIVE_TRISTRIP:
            return MTL::PrimitiveTypeTriangleStrip;
        case PRIMITIVE_TRIFAN:
        case PRIMITIVE_TRIANGLES:
        default:
            return MTL::PrimitiveTypeTriangle;
        }
    }

    /// Map engine CullMode to Metal CullMode.
    inline MTL::CullMode toMetalCullMode(const CullMode cullMode)
    {
        switch (cullMode) {
        case CullMode::CULLFACE_BACK:
            return MTL::CullModeBack;
        case CullMode::CULLFACE_FRONT:
            return MTL::CullModeFront;
        case CullMode::CULLFACE_NONE:
        case CullMode::CULLFACE_FRONTANDBACK:
        default:
            return MTL::CullModeNone;
        }
    }

    /// Create a Depth32Float texture for the back buffer depth attachment.
    inline MTL::Texture* createDepthTexture(MTL::Device* device, const int width, const int height)
    {
        if (!device || width <= 0 || height <= 0) {
            return nullptr;
        }

        auto* desc = MTL::TextureDescriptor::alloc()->init();
        desc->setTextureType(MTL::TextureType2D);
        desc->setPixelFormat(MTL::PixelFormatDepth32Float);
        desc->setWidth(static_cast<NS::UInteger>(width));
        desc->setHeight(static_cast<NS::UInteger>(height));
        desc->setMipmapLevelCount(1);
        desc->setSampleCount(1);
        desc->setStorageMode(MTL::StorageModePrivate);
        desc->setUsage(MTL::TextureUsageRenderTarget);

        auto* texture = device->newTexture(desc);
        desc->release();
        return texture;
    }

    /// Backbuffer sentinel target — preserves JS renderTarget/backBuffer identity semantics.
    class MetalBackBufferRenderTarget final : public RenderTarget
    {
    public:
        explicit MetalBackBufferRenderTarget(const RenderTargetOptions& options)
            : RenderTarget(options) {}

    private:
        void destroyFrameBuffers() override {}
        void createFrameBuffers() override {}
    };
}
