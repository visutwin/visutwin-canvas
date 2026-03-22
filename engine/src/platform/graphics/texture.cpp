// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.09.2025.
//
#include "texture.h"

#include <assert.h>
#include <algorithm>
#include <spdlog/spdlog.h>

#include "graphicsDevice.h"
#include "textureUtils.h"

namespace visutwin::canvas
{
    uint32_t Texture::_nextId = 0;

    Texture::Texture(GraphicsDevice* graphicsDevice, const TextureOptions& options) :
        _device(graphicsDevice), _width(options.width), _height(options.height), _depth(options.depth),
        _format(options.format), _id(_nextId++), _minFilter(options.minFilter), _magFilter(options.magFilter),
        _cubemap(options.cubemap), _volume(options.volume), _arrayLength(options.arrayLength), _numLevelsRequested(options.numLevels),
        _mipmaps(options.mipmaps), _storage(options.storage), _name(options.name), _profilerHint(options.profilerHint)
    {
        assert(options.width > 0 && "Texture width must be greater than 0");
        assert(options.height > 0 && "Texture height must be greater than 0");
        assert(options.depth > 0 && "Texture depth must be greater than 0");

        _compressed = isCompressedPixelFormat(_format);
        _integerFormat = isIntegerPixelFormat(_format);

        if (_integerFormat) {
            _minFilter = FilterMode::FILTER_NEAREST;
            _magFilter = FilterMode::FILTER_NEAREST;
        }

        if (_cubemap) {
            _projection = TextureProjection::TEXTUREPROJECTION_CUBE;
        } else {
            _projection = options.projection;
        }

        // Set the number of levels
        if (options.numLevels > 0) {
            _numLevels = options.numLevels;
        }
        updateNumLevels();

        // Initialize level data
        if (options.levels) {
            // Handle provided level data
            // Note: In actual implementation, would need to properly handle different data types
        } else {
            clearLevels();
        }

        // Create implementation
        recreateImpl(options.levels != nullptr);

        spdlog::trace("Alloc: Id %u %s: %ux%u [Format:%u]%s%s%s[MipLevels:%u]",
                    _id, _name, _width, _height, static_cast<uint32_t>(_format),
                    _cubemap ? "[Cubemap]" : "",
                    _volume ? "[Volume]" : "",
                    isArray() ? "[Array]" : "",
                    _numLevels);
    }

    Texture::~Texture()
    {

    }

    void Texture::updateNumLevels() {
        uint32_t maxLevels = _mipmaps ? TextureUtils::calcMipLevelsCount(_width, _height) : 1;

        if (_numLevelsRequested > 0 && _numLevelsRequested > maxLevels) {
            spdlog::warn("Texture#numLevels: requested mip level count %u is greater than maximum %u, clamping",
                         _numLevelsRequested, maxLevels);
        }

        _numLevels = std::min(_numLevelsRequested > 0 ? _numLevelsRequested : maxLevels, maxLevels);
        _mipmaps = _numLevels > 1;
    }

    void Texture::clearLevels() {
        if (_cubemap) {
            _levels.resize(1);
            _levels[0].resize(6, nullptr);
            _levelDataSizes.resize(1);
            _levelDataSizes[0].resize(6, 0);
            _levelStorage.resize(1);
            _levelStorage[0].resize(6);
            _levelsUpdated.resize(1);
            _levelsUpdated[0].resize(6, true);
        } else {
            _levels.resize(1);
            _levels[0].resize(1, nullptr);
            _levelDataSizes.resize(1);
            _levelDataSizes[0].resize(1, 0);
            _levelStorage.resize(1);
            _levelStorage[0].resize(1);
            _levelsUpdated.resize(1);
            _levelsUpdated[0].resize(1, true);
        }
    }

    void Texture::recreateImpl(bool enableUpload)
    {
        // destroy existing
        _impl.reset();

        // create new
        _impl = _device->createGPUTexture(this);
        dirtyAll();

        if (enableUpload) {
           upload();
        }
    }

    void Texture::dirtyAll() {
        if (_cubemap) {
            _levelsUpdated.resize(1);
            _levelsUpdated[0].resize(6, true);
        } else {
            _levelsUpdated.resize(1);
            _levelsUpdated[0].resize(1, true);
        }

        _needsUpload = true;
        _needsMipmapsUpload = _mipmaps;
        _mipmapsUploaded = false;

        //propertyChanged(TEXPROPERTY_ALL);
    }

    void Texture::upload()
    {
        _needsUpload = true;
        _needsMipmapsUpload = _mipmaps;

        if (_impl) {
            _impl->uploadImmediate(_device);
        }
    }

    void* Texture::getLevel(uint32_t mipLevel) const
    {
        if (_levels.empty() || mipLevel >= _levels[0].size()) {
            return nullptr;
        }
        return _levels[0][mipLevel];
    }

    size_t Texture::getLevelDataSize(uint32_t mipLevel, uint32_t face) const
    {
        if (face >= _levelDataSizes.size() || mipLevel >= _levelDataSizes[face].size()) {
            return 0;
        }
        return _levelDataSizes[face][mipLevel];
    }

    void Texture::setLevelData(uint32_t mipLevel, const uint8_t* data, size_t dataSize, uint32_t face)
    {
        if (!data || dataSize == 0) {
            return;
        }

        if (_cubemap) {
            if (face >= 6) {
                spdlog::warn("Texture::setLevelData face index {} out of range for cubemap", face);
                return;
            }
        } else {
            face = 0;
        }

        if (face >= _levels.size()) {
            _levels.resize(face + 1);
            _levelDataSizes.resize(face + 1);
            _levelStorage.resize(face + 1);
            _levelsUpdated.resize(face + 1);
        }

        const auto requiredLevels = mipLevel + 1;
        if (_levels[face].size() < requiredLevels) {
            _levels[face].resize(requiredLevels, nullptr);
            _levelDataSizes[face].resize(requiredLevels, 0);
            _levelStorage[face].resize(requiredLevels);
            _levelsUpdated[face].resize(requiredLevels, true);
        }

        auto& storage = _levelStorage[face][mipLevel];
        storage.assign(data, data + dataSize);
        _levels[face][mipLevel] = storage.data();
        _levelDataSizes[face][mipLevel] = dataSize;
        _levelsUpdated[face][mipLevel] = true;
        _needsUpload = true;
    }

    void* Texture::getFaceData(uint32_t mipLevel, uint32_t face) const {
        if (!_cubemap || face >= 6 || _levels.size() <= face) {
            return nullptr;
        }
        if (mipLevel >= _levels[face].size()) {
            return nullptr;
        }
        return _levels[face][mipLevel];
    }

    void* Texture::getArrayData(uint32_t mipLevel, uint32_t index) const {
        if (!isArray() || index >= _arrayLength) {
            return nullptr;
        }
        // Note: Array texture implementation would be more complex
        return getLevel(mipLevel);
    }

    void Texture::resize(uint32_t width, uint32_t height, uint32_t depth)
    {
        // Update VRAM tracking
        if (_gpuSize > 0) {
            adjustVramSizeTracking(_device->_vram, -static_cast<int64_t>(_gpuSize));
        }

        // Clear levels
        clearLevels();

        // Update dimensions
        _width = width;
        _height = height;
        _depth = depth;
        updateNumLevels();

        dirtyAll();
    }

    void Texture::adjustVramSizeTracking(DeviceVRAM& vram, int64_t size)
    {
        spdlog::trace("%u %s size: %lld vram.tex: %zu => %zu", _id, _name.c_str(), size, vram.tex, vram.tex + size);

        vram.tex += size;

        // Update profiler-specific VRAM tracking
        switch (_profilerHint) {
        case TexHint::TEXHINT_SHADOWMAP:
            vram.texShadow += size;
            break;
        case TexHint::TEXHINT_ASSET:
            vram.texAsset += size;
            break;
        case TexHint::TEXHINT_LIGHTMAP:
            vram.texLightmap += size;
            break;
        default:
            break;
        }
    }

    void Texture::setMipmaps(bool mipmaps)
    {
        if (_mipmaps != mipmaps) {
            _mipmaps = mipmaps;

            if (mipmaps) {
                _needsMipmapsUpload = true;
            }
        }
    }

    void Texture::setMinFilter(FilterMode filter)
    {
        if (_minFilter != filter) {
            if (_integerFormat) {
                spdlog::warn("minFilter property cannot be changed on integer texture, will remain FILTER_NEAREST");
            } else {
                _minFilter = filter;
                propertyChanged(TEXPROPERTY_MIN_FILTER);
            }
        }
    }

    void Texture::setMagFilter(FilterMode filter)
    {
        if (_magFilter != filter) {
            if (_integerFormat) {
                spdlog::warn("magFilter property cannot be changed on integer texture, will remain FILTER_NEAREST");
            } else {
                _magFilter = filter;
                propertyChanged(TEXPROPERTY_MAG_FILTER);
            }
        }
    }

    void Texture::setAddressU(AddressMode address)
    {
        if (_addressU != address) {
            _addressU = address;
            propertyChanged(TEXPROPERTY_ADDRESS_U);
        }
    }

    void Texture::setAddressV(AddressMode address)
    {
        if (_addressV != address) {
            _addressV = address;
            propertyChanged(TEXPROPERTY_ADDRESS_V);
        }
    }

    void Texture::setAddressW(AddressMode address)
    {
        if (!_volume) {
            spdlog::warn("Cannot set W addressing mode for a non-3D texture");
            return;
        }
        if (_addressW != address) {
            _addressW = address;
            propertyChanged(TEXPROPERTY_ADDRESS_W);
        }
    }

    void Texture::propertyChanged(TextureProperty flag)
    {
        if (_impl)
        {
            _impl->propertyChanged(flag);
        }
        _renderVersionDirty = _device->renderVersion();
    }
}
