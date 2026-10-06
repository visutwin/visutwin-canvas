// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// Shared Metal utility functions and enum converters.
// Extracted from MetalGraphicsDevice to enable reuse across Metal backend classes.
//
#pragma once

#include <cstring>
#include <simd/simd.h>
#include <Metal/Metal.hpp>

#include "core/math/matrix4.h"
#include "platform/graphics/constants.h"
#include "platform/graphics/renderTarget.h"
#include "scene/mesh.h"

namespace visutwin::canvas::metal
{
    /// Convert a column-major Matrix4 to a SIMD float4x4. Every SIMD backend stores
    /// Matrix4 as sixteen contiguous column-major floats, which is float4x4's layout too,
    /// so this is one 64-byte copy whichever backend the engine compiled.
    inline simd::float4x4 toSimdMatrix(const Matrix4& matrix)
    {
        static_assert(sizeof(simd::float4x4) == 64 && sizeof(Matrix4) == 64,
            "toSimdMatrix copies sixteen packed floats");
        simd::float4x4 out;
        std::memcpy(&out, &matrix, sizeof(out));
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

    /// The back buffer's depth: with a STENCIL, which UI masks write and test.
    /// backBufferDepthFormat() reports it, so a depth grab
    /// from the back buffer copies into the same format, as a blit requires.
    inline constexpr MTL::PixelFormat kBackBufferDepthFormat = MTL::PixelFormatDepth32Float_Stencil8;

    /// A back-buffer attachment: `format` at `samples` (2D multisample above 1), private,
    /// render target only.
    inline MTL::Texture* createBackBufferTexture(MTL::Device* device, const MTL::PixelFormat format,
        const int width, const int height, const int samples)
    {
        if (!device || width <= 0 || height <= 0) {
            return nullptr;
        }

        auto* desc = MTL::TextureDescriptor::alloc()->init();
        desc->setTextureType(samples > 1 ? MTL::TextureType2DMultisample : MTL::TextureType2D);
        desc->setPixelFormat(format);
        desc->setWidth(static_cast<NS::UInteger>(width));
        desc->setHeight(static_cast<NS::UInteger>(height));
        desc->setMipmapLevelCount(1);
        desc->setSampleCount(static_cast<NS::UInteger>(samples > 1 ? samples : 1));
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
