// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 08.08.2025.
//

#include "metalTexture.h"

#include <algorithm>
#include <vector>
#include <spdlog/spdlog.h>
#include "metalGraphicsDevice.h"

namespace visutwin::canvas::gpu
{
    namespace
    {
        MTL::PixelFormat pixelFormatToMetal(const PixelFormat format)
        {
            switch (format) {
            case PixelFormat::PIXELFORMAT_RGB8:
                return MTL::PixelFormatRGBA8Unorm;
            case PixelFormat::PIXELFORMAT_RGBA8:
                return MTL::PixelFormatRGBA8Unorm;
            case PixelFormat::PIXELFORMAT_RGBA16F:
                return MTL::PixelFormatRGBA16Float;
            case PixelFormat::PIXELFORMAT_RGBA32F:
                return MTL::PixelFormatRGBA32Float;
            case PixelFormat::PIXELFORMAT_R32F:
                return MTL::PixelFormatR32Float;
            case PixelFormat::PIXELFORMAT_R8:
                return MTL::PixelFormatR8Unorm;
            case PixelFormat::PIXELFORMAT_RG8:
                return MTL::PixelFormatRG8Unorm;
            case PixelFormat::PIXELFORMAT_DEPTH16:
                return MTL::PixelFormatDepth16Unorm;
            case PixelFormat::PIXELFORMAT_DEPTH:
                return MTL::PixelFormatDepth32Float;
            case PixelFormat::PIXELFORMAT_DEPTHSTENCIL:
                return MTL::PixelFormatDepth32Float_Stencil8;
            default:
                return MTL::PixelFormatRGBA8Unorm;
            }
        }

        uint32_t bytesPerPixel(const PixelFormat format)
        {
            switch (format) {
            case PixelFormat::PIXELFORMAT_R8:
                return 1;
            case PixelFormat::PIXELFORMAT_RG8:
                return 2;
            case PixelFormat::PIXELFORMAT_RGB8:
                return 3;
            case PixelFormat::PIXELFORMAT_RGBA8:
                return 4;
            case PixelFormat::PIXELFORMAT_R32F:
                return 4;
            case PixelFormat::PIXELFORMAT_RGBA16F:
                return 8;
            case PixelFormat::PIXELFORMAT_RGBA32F:
                return 16;
            default:
                return 4;
            }
        }
    }

    MetalTexture::MetalTexture(Texture* texture) : _texture(texture)
    {
    }

    MetalTexture::~MetalTexture()
    {
        if (_descriptor) {
            _descriptor->release();
            _descriptor = nullptr;
        }
        if (_metalTexture && _ownsTexture) {
            _metalTexture->release();
            _metalTexture = nullptr;
        }
    }

    void MetalTexture::setExternalTexture(MTL::Texture* externalTexture)
    {
        if (_metalTexture && _ownsTexture) {
            _metalTexture->release();
        }
        _metalTexture = externalTexture;
        _ownsTexture = false;
    }

    void MetalTexture::create(MetalGraphicsDevice* device)
    {
        assert(_texture->width() > 0 && _texture->height() > 0);

        _descriptor = MTL::TextureDescriptor::alloc()->init();
        _descriptor->setPixelFormat(pixelFormatToMetal(_texture->format()));
        _descriptor->setWidth(_texture->width());
        _descriptor->setHeight(_texture->height());
        _descriptor->setMipmapLevelCount(std::max(1u, _texture->getNumLevels()));
        MTL::TextureUsage usage = MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget;
        if (_texture->storage()) {
            usage = usage | MTL::TextureUsageShaderWrite;
        }
        _descriptor->setUsage(usage);
        _descriptor->setStorageMode(MTL::StorageModeShared);

        if (_texture->isCubemap()) {
            _descriptor->setTextureType(MTL::TextureTypeCube);
        } else if (_texture->isVolume()) {
            _descriptor->setTextureType(MTL::TextureType3D);
            _descriptor->setDepth(_texture->depth());
        }

        _metalTexture = device->raw()->newTexture(_descriptor);
    }

    void MetalTexture::uploadImmediate(GraphicsDevice* device)
    {
        // If the CPU-side dimensions have changed (e.g. after Texture::resize()), the
        // existing Metal texture object has stale dimensions.  Recreate it so that render
        // target color/depth buffers actually allocate GPU memory at the new size.
        if (_metalTexture &&
            (_metalTexture->width() != _texture->width() ||
             _metalTexture->height() != _texture->height() ||
             (_texture->isVolume() && _metalTexture->depth() != _texture->depth()))) {
            auto* metalDevice = dynamic_cast<MetalGraphicsDevice*>(device);
            if (metalDevice) {
                if (_descriptor) { _descriptor->release(); _descriptor = nullptr; }
                _metalTexture->release(); _metalTexture = nullptr;
                create(metalDevice);
            }
            _texture->setNeedsUpload(false);
            _texture->setNeedsMipmapsUpload(false);
            return;
        }

        if (_texture->needsUpload() || _texture->needsMipmapsUpload())
        {
            uploadData(device);
            _texture->setNeedsUpload(false);
            _texture->setNeedsMipmapsUpload(false);
        }
    }

    void MetalTexture::uploadData(GraphicsDevice* device)
    {
        (void)device;
        if (!_descriptor || !_metalTexture) {
            spdlog::warn("Texture upload skipped: Metal texture is not initialized");
            return;
        }
        if (!_texture->hasLevels())
        {
            return;
        }

        bool anyUploads = false;
        bool anyLevelMissing = false;
        uint32_t requiredMipLevels = _texture->getNumLevels();

        for (uint32_t mipLevel = 0; mipLevel < requiredMipLevels; mipLevel++)
        {
            auto* mipObject = _texture->getLevel(mipLevel);

            if (mipObject)
            {
                if (_texture->isCubemap())
                {
                    // Handle cubemap faces
                    for (uint32_t face = 0; face < 6; face++)
                    {
                        auto* faceSource = _texture->getFaceData(mipLevel, face);
                        if (faceSource)
                        {
                            const auto sourceSize = _texture->getLevelDataSize(mipLevel, face);
                            uploadRawImage(faceSource, sourceSize, mipLevel, face);
                            anyUploads = true;
                        }
                        else
                        {
                            anyLevelMissing = true;
                        }
                    }
                }
                else if (_texture->isVolume())
                {
                    const auto sourceSize = _texture->getLevelDataSize(mipLevel);
                    uploadVolumeData(mipObject, sourceSize, mipLevel);
                    anyUploads = true;
                }
                else if (_texture->isArray())
                {
                    // Handle texture arrays
                    for (uint32_t index = 0; index < _texture->getArrayLength(); index++)
                    {
                        if (auto* arraySource = _texture->getArrayData(mipLevel, index))
                        {
                            const auto sourceSize = _texture->getLevelDataSize(mipLevel);
                            uploadRawImage(arraySource, sourceSize, mipLevel, index);
                            anyUploads = true;
                        }
                    }
                }
                else
                {
                    // Handle 2D texture
                    const auto sourceSize = _texture->getLevelDataSize(mipLevel);
                    uploadRawImage(mipObject, sourceSize, mipLevel, 0);
                    anyUploads = true;
                }
            }
            else
            {
                anyLevelMissing = true;
            }
        }
    }

    void MetalTexture::uploadRawImage(void* imageData, size_t imageDataSize, uint32_t mipLevel, uint32_t index) const
    {
        if (!imageData) {
            return;
        }
        const auto descriptorMipLevels = std::max(1u, static_cast<uint32_t>(_descriptor->mipmapLevelCount()));
        if (mipLevel >= descriptorMipLevels) {
            spdlog::warn("Texture upload skipped: mip level {} out of descriptor range {}", mipLevel, descriptorMipLevels);
            return;
        }

        const auto mipWidth = std::max(1u, static_cast<uint32_t>(_descriptor->width()) >> mipLevel);
        const auto mipHeight = std::max(1u, static_cast<uint32_t>(_descriptor->height()) >> mipLevel);
        const auto bpp = bytesPerPixel(_texture->format());
        const auto expectedSize = static_cast<size_t>(mipWidth) * static_cast<size_t>(mipHeight) * bpp;
        if (imageDataSize > 0 && imageDataSize < expectedSize) {
            spdlog::warn("Texture upload skipped: level data size {} smaller than expected {}", imageDataSize, expectedSize);
            return;
        }

        const MTL::Region region = MTL::Region(0, 0, 0, mipWidth, mipHeight, 1);
        // For cubemaps, 'index' is the face/slice index (0-5)
        const NS::UInteger slice = _texture->isCubemap() ? index : 0;

        if (_texture->format() == PixelFormat::PIXELFORMAT_RGB8) {
            // Metal does not expose a 24-bit RGB8 texture format for sampling.
            // Expand source RGB data to RGBA when uploading into RGBA8 textures.
            const size_t pixelCount = static_cast<size_t>(mipWidth) * static_cast<size_t>(mipHeight);
            std::vector<uint8_t> expanded(pixelCount * 4u);
            const auto* src = static_cast<const uint8_t*>(imageData);
            for (size_t i = 0; i < pixelCount; ++i) {
                const size_t srcOffset = i * 3u;
                const size_t dstOffset = i * 4u;
                expanded[dstOffset + 0u] = src[srcOffset + 0u];
                expanded[dstOffset + 1u] = src[srcOffset + 1u];
                expanded[dstOffset + 2u] = src[srcOffset + 2u];
                expanded[dstOffset + 3u] = 255u;
            }

            const NS::UInteger bytesPerRow = 4u * mipWidth;
            _metalTexture->replaceRegion(region, mipLevel, slice, expanded.data(), bytesPerRow, 0);
            return;
        }

        const NS::UInteger bytesPerRow = bpp * mipWidth;
        _metalTexture->replaceRegion(region, mipLevel, slice, imageData, bytesPerRow, 0);
    }

    void MetalTexture::uploadVolumeData(void* imageData, size_t imageDataSize, uint32_t mipLevel) const
    {
        if (!imageData) {
            return;
        }
        const auto descriptorMipLevels = std::max(1u, static_cast<uint32_t>(_descriptor->mipmapLevelCount()));
        if (mipLevel >= descriptorMipLevels) {
            spdlog::warn("Volume texture upload skipped: mip level {} out of descriptor range {}", mipLevel, descriptorMipLevels);
            return;
        }

        const auto mipWidth  = std::max(1u, static_cast<uint32_t>(_descriptor->width()) >> mipLevel);
        const auto mipHeight = std::max(1u, static_cast<uint32_t>(_descriptor->height()) >> mipLevel);
        const auto mipDepth  = std::max(1u, static_cast<uint32_t>(_descriptor->depth()) >> mipLevel);
        const auto bpp = bytesPerPixel(_texture->format());

        const auto expectedSize = static_cast<size_t>(mipWidth) * mipHeight * mipDepth * bpp;
        if (imageDataSize > 0 && imageDataSize < expectedSize) {
            spdlog::warn("Volume texture upload skipped: data size {} < expected {}", imageDataSize, expectedSize);
            return;
        }

        const MTL::Region region = MTL::Region(0, 0, 0, mipWidth, mipHeight, mipDepth);
        const NS::UInteger bytesPerRow   = static_cast<NS::UInteger>(bpp) * mipWidth;
        const NS::UInteger bytesPerImage = bytesPerRow * mipHeight;

        _metalTexture->replaceRegion(region, mipLevel, 0, imageData, bytesPerRow, bytesPerImage);
    }

    void MetalTexture::propertyChanged(uint32_t flag)
    {
        // Clear samplers to force recreation
        for (auto* sampler : _samplers) {
            sampler->release();
        }
        _samplers.clear();
    }
}
