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
     * Copy a rectangle out of ANY Metal texture into `out`, tightly packed at
     * `bytesPerPixel`, blocking until the GPU has finished with it.
     *
     * The copy goes through a shared-storage staging texture unconditionally. A
     * render target is device-private on Apple Silicon, and `getBytes` on a
     * private texture does not fail — it returns whatever is there, which is how
     * `tools/generate-env-atlas` wrote a plausible-looking wrong image for as
     * long as it existed. Paying one blit for a texture that happened to be
     * shared is the cheaper mistake.
     *
     * The back-buffer screenshot reads the DRAWABLE, which is an `MTL::Texture`
     * with no `Texture` in front of it, so this takes the native handle rather
     * than a `MetalTexture` and both paths share one implementation.
     */
    bool readMetalTexture(MTL::Device* device, MTL::CommandQueue* queue,
        MTL::Texture* source, const TextureReadRegion& region,
        uint32_t bytesPerPixel, uint8_t* out, size_t outSize);

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

        bool read(GraphicsDevice* device, const TextureReadRegion& region,
            uint8_t* out, size_t outSize) override;

        // Upload texture data to GPU
        void uploadData(GraphicsDevice* device);

    private:
        void uploadRawImage(void* imageData, size_t imageDataSize, uint32_t mipLevel, uint32_t index) const;
        /// replaceRegion for a shared texture, a staged blit for a private one.
        void writeRegion(const MTL::Region& region, uint32_t mipLevel, NS::UInteger slice,
            const void* data, NS::UInteger bytesPerRow) const;
        void uploadVolumeData(void* imageData, size_t imageDataSize, uint32_t mipLevel) const;

        Texture* _texture = nullptr;

        MTL::Texture* _metalTexture = nullptr;
        bool _ownsTexture = true;  ///< false when using setExternalTexture()

        MTL::TextureDescriptor* _descriptor = nullptr;

        // Array of samplers addressed by sample type
        std::vector<MTL::SamplerState*> _samplers;
    };
}
