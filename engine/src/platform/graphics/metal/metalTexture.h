// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 08.08.2025.
//
#pragma once

#include "metalGraphicsDevice.h"
#include "Metal/Metal.hpp"
#include "platform/graphics/texture.h"

namespace visutwin::canvas::gpu
{
    /**
      * Metal texture implementation.
      * Wraps MTL::Texture and provides texture management functionality.
      */
    class MetalTexture : public HardwareTexture {
    public:
        explicit MetalTexture(Texture* texture);

        ~MetalTexture();

        void create(MetalGraphicsDevice* device);

        [[nodiscard]] MTL::Texture* raw() const { return _metalTexture; }

        /// Replace the underlying Metal texture with an externally-owned one.
        /// Used by MetalTextureStream to inject the latest frame into the
        /// material/rendering system. The external texture is NOT owned by
        /// MetalTexture — caller must ensure it outlives usage.
        void setExternalTexture(MTL::Texture* externalTexture);

        void uploadImmediate(GraphicsDevice* device) override;

        void propertyChanged(uint32_t flag) override;

        // Upload texture data to GPU
        void uploadData(GraphicsDevice* device);

    private:
        void uploadRawImage(void* imageData, size_t imageDataSize, uint32_t mipLevel, uint32_t index) const;
        void uploadVolumeData(void* imageData, size_t imageDataSize, uint32_t mipLevel) const;

        Texture* _texture = nullptr;

        MTL::Texture* _metalTexture = nullptr;
        bool _ownsTexture = true;  ///< false when using setExternalTexture()

        MTL::TextureDescriptor* _descriptor = nullptr;

        // Array of samplers addressed by sample type
        std::vector<MTL::SamplerState*> _samplers;
    };
}
