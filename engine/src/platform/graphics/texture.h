// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.09.2025.
//
#pragma once

#include <memory>
#include <cstddef>
#include <vector>

#include "constants.h"
#include "gpu.h"

namespace visutwin::canvas
{
    enum class TextureEncoding
    {
        Default = 0,
        RGBP = 1,
        RGBM = 2
    };

    class GraphicsDevice;
    struct DeviceVRAM;

    struct TextureOptions
    {
        uint32_t width = 4;
        uint32_t height = 4;
        uint32_t depth = 1;
        PixelFormat format = PixelFormat::PIXELFORMAT_RGBA8;
        FilterMode minFilter = FilterMode::FILTER_LINEAR_MIPMAP_LINEAR;
        FilterMode magFilter = FilterMode::FILTER_LINEAR;
        bool cubemap = false;
        bool volume = false;
        uint32_t arrayLength = 0;
        TextureProjection projection = TextureProjection::TEXTUREPROJECTION_NONE;
        uint32_t numLevels = 0;
        bool mipmaps = true;
        bool storage = false;
        void* levels = nullptr;
        std::string name;
        TexHint profilerHint = TexHint::TEXHINT_NONE;
    };

    /**
     * Represents a texture, which is typically an image composed of pixels (texels).
     * Textures are fundamental resources for rendering graphical objects.
     **/
    class Texture : public std::enable_shared_from_this<Texture>
    {
    public:
        explicit Texture(GraphicsDevice* graphicsDevice, const TextureOptions& options = TextureOptions{});

        ~Texture();

        uint32_t width() const { return _width; }

        uint32_t height() const { return _height; }

        uint32_t depth() const { return _depth; }

        bool isArray() const { return _arrayLength > 0; }

        void upload();

        bool needsUpload() const { return _needsUpload; }
        bool needsMipmapsUpload() const { return _needsMipmapsUpload; }

        void setNeedsUpload(const bool needsUpload) { _needsUpload = needsUpload; }
        void setNeedsMipmapsUpload(const bool needsMipmapsUpload) { _needsMipmapsUpload = needsMipmapsUpload; }

        bool hasLevels() const { return !_levels.empty(); }
        uint32_t getNumLevels() const { return _numLevels; }
        void* getLevel(uint32_t mipLevel) const;
        size_t getLevelDataSize(uint32_t mipLevel, uint32_t face = 0) const;
        void setLevelData(uint32_t mipLevel, const uint8_t* data, size_t dataSize, uint32_t face = 0);

        bool isCubemap() const { return _cubemap; }

        void* getFaceData(uint32_t mipLevel, uint32_t face) const;
        void* getArrayData(uint32_t mipLevel, uint32_t index) const;

        bool isVolume() const { return _volume; }

        uint32_t getArrayLength() const { return _arrayLength; }

        bool mipmaps() const { return _mipmaps; }
        void setMipmaps(bool mipmaps);

        PixelFormat format() const { return _format; }
        bool storage() const { return _storage; }

        GraphicsDevice* device() const { return _device; }
        gpu::HardwareTexture* impl() const { return _impl.get(); }

        const std::string& name() const { return _name; }
        TextureEncoding encoding() const { return _encoding; }
        void setEncoding(TextureEncoding value) { _encoding = value; }

        // Resize the texture
        void resize(uint32_t width, uint32_t height, uint32_t depth = 1);

        void setMinFilter(FilterMode filter);

        void setMagFilter(FilterMode filter);

        void setAddressU(AddressMode address);

        void setAddressV(AddressMode address);

        void setAddressW(AddressMode address);

    protected:
        virtual void propertyChanged(TextureProperty flag);

    private:
        void updateNumLevels();
        void clearLevels();

        void recreateImpl(bool enableUpload = true);

        void dirtyAll();

        // Update VRAM size tracking
        void adjustVramSizeTracking(DeviceVRAM& vram, int64_t size);

        static uint32_t _nextId;

        std::string _name;
        TextureEncoding _encoding = TextureEncoding::Default;

        GraphicsDevice* _device;

        uint32_t _width;
        uint32_t _height;
        uint32_t _depth;
        uint32_t _arrayLength;

        PixelFormat _format;

        uint32_t _id;

        bool _compressed;
        bool _integerFormat;

        FilterMode _minFilter;
        FilterMode _magFilter;

        bool _cubemap;
        bool _volume;
        bool _mipmaps;
        bool _storage = false;

        TextureProjection _projection;

        uint32_t _numLevels = 0;
        uint32_t _numLevelsRequested;

        std::vector<std::vector<void*>> _levels;
        std::vector<std::vector<size_t>> _levelDataSizes;
        std::vector<std::vector<std::vector<uint8_t>>> _levelStorage;
        std::vector<std::vector<bool>> _levelsUpdated;

        std::unique_ptr<gpu::HardwareTexture> _impl;

        bool _needsUpload = false;
        bool _needsMipmapsUpload = false;
        bool _mipmapsUploaded = false;

        size_t _gpuSize = 0;

        TexHint _profilerHint;

        AddressMode _addressU = ADDRESS_REPEAT;
        AddressMode _addressV = ADDRESS_REPEAT;
        AddressMode _addressW = ADDRESS_REPEAT;

        uint32_t _renderVersionDirty = 0; // Version tracking
    };
}
