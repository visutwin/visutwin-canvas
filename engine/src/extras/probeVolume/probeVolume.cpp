// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
#include "probeVolume.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

#include "core/math/halfFloat.h"
#include "platform/graphics/bindlessResidency.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/textureSamplerState.h"
#include "scene/materials/material.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    namespace
    {
        // The serialized data: its magic number ('PCPV') and version, the size of its header,
        // and the flags of its optional parts.
        constexpr uint32_t kMagic = 0x56504350u;
        constexpr uint32_t kVersion = 3u;
        constexpr size_t kHeaderSize = 56;
        constexpr uint32_t kFlagValidity = 1u;
        constexpr uint32_t kFlagOffsets = 2u;
        constexpr uint32_t kFlagVisibility = 4u;

        const float kSqrt3 = std::sqrt(3.0f);

        template <typename T>
        void writeValue(std::vector<uint8_t>& buffer, const size_t offset, const T value)
        {
            std::memcpy(buffer.data() + offset, &value, sizeof(T));
        }

        template <typename T>
        T readValue(const std::span<const uint8_t> buffer, const size_t offset)
        {
            T value;
            std::memcpy(&value, buffer.data() + offset, sizeof(T));
            return value;
        }

        // The bindless lane a shader reads a texture through: its index, and its own sampler.
        uint32_t laneOf(const Texture* texture)
        {
            if (!texture) {
                return 0u;
            }
            return packBindlessLane(texture->bindlessIndex(),
                BindlessSamplers::indexOf(textureSamplerState(*texture, 1.0f)));
        }

        void setClamp(Texture& texture)
        {
            texture.setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            texture.setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            texture.setAddressW(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        }
    }

    ProbeVolume::ProbeVolume(GraphicsDevice* device, const BoundingBox& aabb,
        const std::array<uint32_t, 3>& resolution, const PixelFormat format)
        : _device(device), _aabb(aabb), _resolution(resolution), _format(format)
    {
        for (uint32_t& count : _resolution) {
            if (count < 1u) {
                spdlog::warn("ProbeVolume: the resolution needs at least one probe on each axis");
                count = 1u;
            }
        }
        const auto corner = [&](const int axis, const double sign) {
            return static_cast<float>(static_cast<double>(_aabb.center()[axis]) +
                sign * static_cast<double>(_aabb.halfExtents()[axis]));
        };
        _min = Vector3(corner(0, -1.0), corner(1, -1.0), corner(2, -1.0));
        _max = Vector3(corner(0, 1.0), corner(1, 1.0), corner(2, 1.0));
        _lightings.emplace_back(static_cast<size_t>(probeCount()) * kFloatsPerProbe, 0.0f);
        _validity.assign(probeCount(), 1u);

        TextureOptions options;
        options.name = "ProbeVolumeValidity";
        options.width = _resolution[0];
        options.height = _resolution[1];
        options.depth = _resolution[2];
        options.volume = true;
        options.format = PixelFormat::PIXELFORMAT_RGBA8;
        options.mipmaps = false;
        options.minFilter = FilterMode::FILTER_NEAREST;
        options.magFilter = FilterMode::FILTER_NEAREST;
        _validityTexture = std::make_unique<Texture>(_device, options);
        setClamp(*_validityTexture);

        updateTexture();
        uploadValidity();
        updateVisibilityTexture();
        updateUniforms();
    }

    ProbeVolume::~ProbeVolume()
    {
        if (_device) {
            _device->clearProbeVolumeBlocks(this);
        }
    }

    void ProbeVolume::setFormat(const PixelFormat format)
    {
        if (_format != format) {
            _format = format;
            updateTexture();
            updateUniforms();
        }
    }

    void ProbeVolume::setActiveLighting(const uint32_t index)
    {
        if (index >= _lightings.size()) {
            spdlog::warn("ProbeVolume::setActiveLighting: no lighting {}", index);
            return;
        }
        if (_activeLighting != index) {
            _activeLighting = index;
            uploadData();
            updateUniforms();
        }
    }

    std::span<const float> ProbeVolume::data(const uint32_t index) const
    {
        if (index >= _lightings.size()) {
            spdlog::warn("ProbeVolume::data: no lighting {}", index);
            return {};
        }
        return _lightings[index];
    }

    void ProbeVolume::setData(const std::span<const float> data, const uint32_t index)
    {
        if (index > _lightings.size()) {
            spdlog::warn("ProbeVolume::setData: the index {} is beyond the {} lightings", index, _lightings.size());
            return;
        }
        if (data.size() != _lightings[0].size()) {
            spdlog::warn("ProbeVolume::setData: expected {} floats, got {}", _lightings[0].size(), data.size());
            return;
        }
        if (index == _lightings.size()) {
            _lightings.emplace_back(data.size(), 0.0f);
        }
        std::copy(data.begin(), data.end(), _lightings[index].begin());
        _hasData = true;
        if (index == _activeLighting) {
            uploadData();
        }
        updateUniforms();
    }

    void ProbeVolume::setBakeData(const std::span<const float> data)
    {
        if (!data.empty()) {
            if (data.size() != _lightings[0].size()) {
                spdlog::warn("ProbeVolume::setBakeData: expected {} floats, got {}", _lightings[0].size(), data.size());
                return;
            }
            if (!_captureTexture) {
                _captureTexture = createProbeTexture("ProbeVolumeCapture", PixelFormat::PIXELFORMAT_RGBA16F);
            }
            encodeProbes(*_captureTexture, data, false);
        } else {
            _captureTexture.reset();
            _bakeGeometry = ProbeVolumeBlock{};
        }
        updateUniforms();
    }

    void ProbeVolume::setBakeGeometry(const ProbeVolumeBlock& source)
    {
        std::copy(std::begin(source.bakeLanes), std::end(source.bakeLanes), std::begin(_bakeGeometry.bakeLanes));
        std::copy(std::begin(source.bakeSkyLane), std::end(source.bakeSkyLane), std::begin(_bakeGeometry.bakeSkyLane));
        std::copy(std::begin(source.bakeAtlas), std::end(source.bakeAtlas), std::begin(_bakeGeometry.bakeAtlas));
        updateUniforms();
    }

    void ProbeVolume::setValidity(const std::span<const uint8_t> validity)
    {
        if (!validity.empty()) {
            if (validity.size() != probeCount()) {
                spdlog::warn("ProbeVolume::setValidity: expected {} values, got {}", probeCount(), validity.size());
                return;
            }
            for (size_t i = 0; i < validity.size(); ++i) {
                _validity[i] = validity[i] != 0 ? 1u : 0u;
            }
        } else {
            std::fill(_validity.begin(), _validity.end(), uint8_t{1});
        }
        uploadValidity();
    }

    uint32_t ProbeVolume::invalidProbeCount() const
    {
        return static_cast<uint32_t>(std::count(_validity.begin(), _validity.end(), uint8_t{0}));
    }

    void ProbeVolume::setOffsets(const std::span<const float> offsets)
    {
        _offsets.assign(offsets.begin(), offsets.end());
        uploadValidity();
    }

    void ProbeVolume::setVisibility(const std::span<const float> data, const uint32_t size)
    {
        if (!data.empty()) {
            const size_t expected = static_cast<size_t>(probeCount()) * size * size * 2;
            if (size == 0 || data.size() != expected) {
                spdlog::warn("ProbeVolume::setVisibility: expected {} floats, got {}", expected, data.size());
                return;
            }
            _visibility.assign(data.begin(), data.end());
            _visibilitySize = size;
        } else {
            _visibility.clear();
            _visibilitySize = 0;
        }
        updateVisibilityTexture();
        updateUniforms();
    }

    void ProbeVolume::setNormalBias(const float value) { _normalBias = value; updateUniforms(); }
    void ProbeVolume::setIntensity(const float value) { _intensity = value; updateUniforms(); }
    void ProbeVolume::setShSharpening(const bool value) { _shSharpening = value; updateUniforms(); }
    void ProbeVolume::setLeakReduction(const bool value) { _leakReduction = value; updateUniforms(); }
    void ProbeVolume::setVisibilityEnabled(const bool value) { _useVisibility = value; updateUniforms(); }
    void ProbeVolume::setReflectionOcclusion(const bool value) { _reflectionOcclusion = value; updateUniforms(); }
    void ProbeVolume::setBlendDistance(const float value) { _blendDistance = value; updateUniforms(); }
    void ProbeVolume::setEnabled(const bool value) { _enabled = value; updateUniforms(); }

    Vector3 ProbeVolume::probePosition(const uint32_t index) const
    {
        const auto [nx, ny, nz] = _resolution;
        const uint32_t ix = index % nx;
        const uint32_t iy = (index / nx) % ny;
        const uint32_t iz = index / (nx * ny);
        const Vector3 min = aabbMin();
        const Vector3 max = aabbMax();
        return Vector3(
            min.getX() + (max.getX() - min.getX()) * (static_cast<float>(ix) + 0.5f) / static_cast<float>(nx),
            min.getY() + (max.getY() - min.getY()) * (static_cast<float>(iy) + 0.5f) / static_cast<float>(ny),
            min.getZ() + (max.getZ() - min.getZ()) * (static_cast<float>(iz) + 0.5f) / static_cast<float>(nz));
    }

    Vector3 ProbeVolume::cellSize() const
    {
        const Vector3& half = _aabb.halfExtents();
        return Vector3(2.0f * half.getX() / static_cast<float>(_resolution[0]),
            2.0f * half.getY() / static_cast<float>(_resolution[1]),
            2.0f * half.getZ() / static_cast<float>(_resolution[2]));
    }

    void ProbeVolume::applyToMaterial(Material& material, const bool apply)
    {
        material.setProbeVolume(apply);
    }

    std::vector<uint8_t> ProbeVolume::serialize() const
    {
        const uint32_t count = probeCount();
        const size_t validityBytes = (static_cast<size_t>(count) + 3) / 4 * 4;
        const size_t offsetsBytes = _offsets.size() * sizeof(float);
        const size_t visibilityBytes = _visibility.empty() ? 0 : 4 + _visibility.size() * 2;
        const size_t lightingBytes = static_cast<size_t>(count) * kFloatsPerProbe * sizeof(float);
        std::vector<uint8_t> buffer(kHeaderSize + validityBytes + offsetsBytes + visibilityBytes +
            lightingBytes * _lightings.size(), 0u);

        const Vector3 min = aabbMin();
        const Vector3 max = aabbMax();
        writeValue(buffer, 0, kMagic);
        writeValue(buffer, 4, kVersion);
        for (int i = 0; i < 3; ++i) {
            writeValue(buffer, 8 + i * 4, _resolution[i]);
        }
        const float bounds[6] = {min.getX(), min.getY(), min.getZ(), max.getX(), max.getY(), max.getZ()};
        for (int i = 0; i < 6; ++i) {
            writeValue(buffer, 20 + i * 4, bounds[i]);
        }
        writeValue(buffer, 44, static_cast<uint32_t>(kFloatsPerProbe));
        writeValue(buffer, 48, kFlagValidity | (_offsets.empty() ? 0u : kFlagOffsets) |
            (_visibility.empty() ? 0u : kFlagVisibility));
        writeValue(buffer, 52, static_cast<uint32_t>(_lightings.size()));

        size_t offset = kHeaderSize;
        std::memcpy(buffer.data() + offset, _validity.data(), count);
        offset += validityBytes;
        if (!_offsets.empty()) {
            std::memcpy(buffer.data() + offset, _offsets.data(), offsetsBytes);
            offset += offsetsBytes;
        }
        if (!_visibility.empty()) {
            writeValue(buffer, offset, _visibilitySize);
            for (size_t i = 0; i < _visibility.size(); ++i) {
                writeValue(buffer, offset + 4 + i * 2, floatToHalf(_visibility[i]));
            }
            offset += visibilityBytes;
        }
        // The lightings after the data they share; the visibility, an even number of halves,
        // keeps the floats aligned.
        for (const auto& lighting : _lightings) {
            std::memcpy(buffer.data() + offset, lighting.data(), lightingBytes);
            offset += lightingBytes;
        }
        return buffer;
    }

    std::unique_ptr<ProbeVolume> ProbeVolume::deserialize(GraphicsDevice* device, const std::span<const uint8_t> buffer,
        const PixelFormat format)
    {
        if (buffer.size() < kHeaderSize || readValue<uint32_t>(buffer, 0) != kMagic ||
            readValue<uint32_t>(buffer, 4) != kVersion) {
            spdlog::error("ProbeVolume::deserialize: unknown data");
            return nullptr;
        }
        if (readValue<uint32_t>(buffer, 44) != static_cast<uint32_t>(kFloatsPerProbe)) {
            spdlog::error("ProbeVolume::deserialize: unsupported probe data");
            return nullptr;
        }
        std::array<uint32_t, 3> resolution{};
        for (int i = 0; i < 3; ++i) {
            resolution[i] = readValue<uint32_t>(buffer, 8 + i * 4);
        }
        float v[6];
        for (int i = 0; i < 6; ++i) {
            v[i] = readValue<float>(buffer, 20 + i * 4);
        }
        const uint32_t flags = readValue<uint32_t>(buffer, 48);
        const uint32_t lightingCount = readValue<uint32_t>(buffer, 52);
        if (lightingCount < 1u) {
            spdlog::error("ProbeVolume::deserialize: the data has no lighting");
            return nullptr;
        }

        // The size the parts need, before reading any of them.
        const size_t count = static_cast<size_t>(resolution[0]) * resolution[1] * resolution[2];
        size_t needed = kHeaderSize + ((flags & kFlagValidity) ? (count + 3) / 4 * 4 : 0) +
            ((flags & kFlagOffsets) ? count * 3 * sizeof(float) : 0);
        uint32_t visibilitySize = 0;
        if (flags & kFlagVisibility) {
            if (buffer.size() < needed + 4) {
                spdlog::error("ProbeVolume::deserialize: the data is truncated");
                return nullptr;
            }
            visibilitySize = readValue<uint32_t>(buffer, needed);
            needed += 4 + count * visibilitySize * visibilitySize * 2 * 2;
        }
        needed += lightingCount * count * kFloatsPerProbe * sizeof(float);
        if (count == 0 || buffer.size() < needed) {
            spdlog::error("ProbeVolume::deserialize: the data is truncated");
            return nullptr;
        }

        // The box from its corners in double precision, and the corners kept as they were saved.
        const auto mid = [&](const int axis, const double sign) {
            return static_cast<float>((static_cast<double>(v[axis + 3]) + sign * static_cast<double>(v[axis])) * 0.5);
        };
        const BoundingBox aabb(Vector3(mid(0, 1.0), mid(1, 1.0), mid(2, 1.0)),
            Vector3(mid(0, -1.0), mid(1, -1.0), mid(2, -1.0)));
        auto volume = std::make_unique<ProbeVolume>(device, aabb, resolution, format);
        volume->_min = Vector3(v[0], v[1], v[2]);
        volume->_max = Vector3(v[3], v[4], v[5]);
        volume->updateUniforms();

        size_t offset = kHeaderSize;
        if (flags & kFlagValidity) {
            volume->setValidity(buffer.subspan(offset, count));
            offset += (count + 3) / 4 * 4;
        }
        if (flags & kFlagOffsets) {
            std::vector<float> offsets(count * 3);
            std::memcpy(offsets.data(), buffer.data() + offset, offsets.size() * sizeof(float));
            volume->setOffsets(offsets);
            offset += offsets.size() * sizeof(float);
        }
        if (flags & kFlagVisibility) {
            std::vector<float> visibility(count * visibilitySize * visibilitySize * 2);
            for (size_t i = 0; i < visibility.size(); ++i) {
                visibility[i] = halfToFloat(readValue<uint16_t>(buffer, offset + 4 + i * 2));
            }
            volume->setVisibility(visibility, visibilitySize);
            offset += 4 + visibility.size() * 2;
        }
        std::vector<float> lighting(count * kFloatsPerProbe);
        for (uint32_t i = 0; i < lightingCount; ++i) {
            std::memcpy(lighting.data(), buffer.data() + offset, lighting.size() * sizeof(float));
            volume->setData(lighting, i);
            offset += lighting.size() * sizeof(float);
        }
        return volume;
    }

    std::unique_ptr<Texture> ProbeVolume::createProbeTexture(const char* name, const PixelFormat format) const
    {
        TextureOptions options;
        options.name = name;
        options.width = _resolution[0];
        options.height = _resolution[1];
        options.depth = _resolution[2] * 3;
        options.volume = true;
        options.format = format;
        options.mipmaps = false;
        options.minFilter = FilterMode::FILTER_LINEAR;
        options.magFilter = FilterMode::FILTER_LINEAR;
        auto texture = std::make_unique<Texture>(_device, options);
        setClamp(*texture);
        return texture;
    }

    void ProbeVolume::updateTexture()
    {
        _texture = createProbeTexture("ProbeVolume", _format);
        uploadData();
    }

    void ProbeVolume::uploadData()
    {
        const std::vector<float>& data = _lightings[_activeLighting];
        const bool eightBit = _format == PixelFormat::PIXELFORMAT_RGBA8;

        // 8-bit texels store L0 relative to the brightest L0 of the probes.
        if (eightBit) {
            float maxL0 = 0.0f;
            for (size_t i = 0; i < data.size(); i += 4) {
                maxL0 = std::max(maxL0, data[i]);
            }
            _l0Scale = std::max(maxL0, 1e-6f);
        }
        encodeProbes(*_texture, data, eightBit);
    }

    void ProbeVolume::encodeProbes(Texture& texture, const std::span<const float> data, const bool eightBit) const
    {
        const auto [nx, ny, nz] = _resolution;
        const uint32_t count = probeCount();
        const size_t texelCount = static_cast<size_t>(count) * 3;

        std::vector<uint8_t> bytes;
        std::vector<uint16_t> halves;
        if (eightBit) {
            bytes.resize(texelCount * 4);
        } else {
            halves.resize(texelCount * 4);
        }
        for (uint32_t probe = 0; probe < count; ++probe) {
            const uint32_t ix = probe % nx;
            const uint32_t iy = (probe / nx) % ny;
            const uint32_t iz = probe / (nx * ny);
            for (uint32_t channel = 0; channel < 3; ++channel) {
                const size_t src = (static_cast<size_t>(probe) * 3 + channel) * 4;
                const size_t dst = (ix + nx * (iy + ny * (iz + static_cast<size_t>(nz) * channel))) * 4;
                const float l0 = data[src];
                if (eightBit) {
                    // sqrt(L0 / scale), and L1 relative to its largest magnitude, sqrt(3) L0.
                    const float invL1 = l0 > 0.0f ? 1.0f / (kSqrt3 * l0) : 0.0f;
                    bytes[dst] = static_cast<uint8_t>(std::lround(std::sqrt(std::max(l0, 0.0f) / _l0Scale) * 255.0f));
                    for (int i = 1; i < 4; ++i) {
                        const float ratio = std::clamp(data[src + i] * invL1, -1.0f, 1.0f);
                        bytes[dst + i] = static_cast<uint8_t>(std::lround((ratio * 0.5f + 0.5f) * 255.0f));
                    }
                } else {
                    for (int i = 0; i < 4; ++i) {
                        halves[dst + i] = floatToHalf(data[src + i]);
                    }
                }
            }
        }
        if (eightBit) {
            texture.setLevelData(0, bytes.data(), bytes.size());
        } else {
            texture.setLevelData(0, reinterpret_cast<const uint8_t*>(halves.data()), halves.size() * sizeof(uint16_t));
        }
        texture.upload();
    }

    void ProbeVolume::uploadValidity()
    {
        if (!_validityTexture) {
            return;
        }
        const auto [nx, ny, nz] = _resolution;
        // The probes outside the grid count as valid: the border cells do not interpolate them.
        const auto isValid = [&](const uint32_t x, const uint32_t y, const uint32_t z) {
            return x >= nx || y >= ny || z >= nz || _validity[x + nx * (y + ny * z)] != 0;
        };
        const Vector3 cellVector = cellSize();
        const float cell[3] = {cellVector.getX(), cellVector.getY(), cellVector.getZ()};

        std::vector<uint8_t> texels(static_cast<size_t>(probeCount()) * 4);
        for (uint32_t z = 0; z < nz; ++z) {
            for (uint32_t y = 0; y < ny; ++y) {
                for (uint32_t x = 0; x < nx; ++x) {
                    const size_t probe = x + nx * (y + ny * z);
                    uint32_t mask = 0;
                    for (uint32_t corner = 0; corner < 8; ++corner) {
                        if (isValid(x + (corner & 1u), y + ((corner >> 1) & 1u), z + ((corner >> 2) & 1u))) {
                            mask |= 1u << corner;
                        }
                    }
                    // Each component of the offset in units of its cell, -0.5..0.5 mapped to 1..255.
                    for (int axis = 0; axis < 3; ++axis) {
                        const float offset = !_offsets.empty() && cell[axis] > 0.0f
                            ? _offsets[probe * 3 + axis] / cell[axis] : 0.0f;
                        texels[probe * 4 + axis] =
                            static_cast<uint8_t>(128 + std::lround(std::clamp(offset, -0.5f, 0.5f) * 254.0f));
                    }
                    texels[probe * 4 + 3] = static_cast<uint8_t>(mask);
                }
            }
        }
        _validityTexture->setLevelData(0, texels.data(), texels.size());
        _validityTexture->upload();
    }

    void ProbeVolume::updateVisibilityTexture()
    {
        const uint32_t size = _visibilitySize;
        const uint32_t tile = size + 2;
        const uint32_t count = probeCount();
        const bool has = !_visibility.empty();
        const uint32_t tilesPerRow = has ? static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(count)))) : 1u;
        const uint32_t width = has ? tilesPerRow * tile : 1u;
        const uint32_t height = has ? (count + tilesPerRow - 1) / tilesPerRow * tile : 1u;
        _visibilityTilesPerRow = tilesPerRow;

        TextureOptions options;
        options.name = "ProbeVolumeVisibility";
        options.width = width;
        options.height = height;
        // DEVIATION: RGBA16F with blue and alpha unused, where upstream stores RG16F, a format
        // this engine does not have.
        options.format = PixelFormat::PIXELFORMAT_RGBA16F;
        options.mipmaps = false;
        options.minFilter = FilterMode::FILTER_LINEAR;
        options.magFilter = FilterMode::FILTER_LINEAR;
        _visibilityTexture = std::make_unique<Texture>(_device, options);
        _visibilityTexture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        _visibilityTexture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);

        std::vector<uint16_t> texels(static_cast<size_t>(width) * height * 4, 0u);
        if (has) {
            for (uint32_t probe = 0; probe < count; ++probe) {
                const uint32_t x0 = (probe % tilesPerRow) * tile;
                const uint32_t y0 = (probe / tilesPerRow) * tile;
                for (uint32_t y = 0; y < tile; ++y) {
                    for (uint32_t x = 0; x < tile; ++x) {
                        // The texel of the map; across an edge of the octahedral map, the texel
                        // mirrored along the edge.
                        int i = static_cast<int>(x) - 1;
                        int j = static_cast<int>(y) - 1;
                        const int last = static_cast<int>(size) - 1;
                        if (i < 0 || i > last) {
                            i = i < 0 ? 0 : last;
                            j = last - j;
                        }
                        if (j < 0 || j > last) {
                            j = j < 0 ? 0 : last;
                            i = last - i;
                        }
                        const size_t src = ((static_cast<size_t>(probe) * size + j) * size + i) * 2;
                        const size_t dst = ((static_cast<size_t>(y0) + y) * width + x0 + x) * 4;
                        texels[dst] = floatToHalf(_visibility[src]);
                        texels[dst + 1] = floatToHalf(_visibility[src + 1]);
                    }
                }
            }
        }
        _visibilityTexture->setLevelData(0, reinterpret_cast<const uint8_t*>(texels.data()),
            texels.size() * sizeof(uint16_t));
        _visibilityTexture->upload();
    }

    void ProbeVolume::updateUniforms()
    {
        const Vector3 min = aabbMin();
        const Vector3 max = aabbMax();
        const auto [nx, ny, nz] = _resolution;
        const auto inverse = [](const float size) { return size > 0.0f ? 1.0f / size : 0.0f; };

        ProbeVolumeBlock& block = _display;
        block = ProbeVolumeBlock{};
        min.store(block.boxMin);
        max.store(block.boxMax);
        block.boxMin[3] = 0.0f;
        block.boxMax[3] = 0.0f;
        const Vector3 size = max - min;
        block.invSize[0] = inverse(size.getX());
        block.invSize[1] = inverse(size.getY());
        block.invSize[2] = inverse(size.getZ());
        block.count[0] = static_cast<float>(nx);
        block.count[1] = static_cast<float>(ny);
        block.count[2] = static_cast<float>(nz);
        block.cellSize[0] = size.getX() / static_cast<float>(nx);
        block.cellSize[1] = size.getY() / static_cast<float>(ny);
        block.cellSize[2] = size.getZ() / static_cast<float>(nz);
        block.params[0] = _normalBias;
        block.params[1] = _intensity;
        block.params[2] = _shSharpening ? 1.0f : 0.0f;
        block.params[3] = _leakReduction ? 1.0f : 0.0f;
        block.blend[0] = _blendDistance > 0.0f ? 1.0f / _blendDistance : 1e6f;
        block.blend[1] = _enabled && _hasData ? 1.0f : 0.0f;
        block.blend[2] = _reflectionOcclusion ? 1.0f : 0.0f;
        block.encoding[0] = _format == PixelFormat::PIXELFORMAT_RGBA8 ? 1.0f : 0.0f;
        block.encoding[1] = _l0Scale;
        block.visibility[0] = _useVisibility && !_visibility.empty() ? 1.0f : 0.0f;
        block.visibility[1] = static_cast<float>(_visibilityTilesPerRow);
        block.visibility[2] = static_cast<float>(_visibilitySize);
        block.visibilityInvSize[0] = 1.0f / static_cast<float>(_visibilityTexture->width());
        block.visibilityInvSize[1] = 1.0f / static_cast<float>(_visibilityTexture->height());
        block.lanes[0] = laneOf(_texture.get());
        block.lanes[1] = laneOf(_validityTexture.get());
        block.lanes[2] = laneOf(_visibilityTexture.get());

        // The cameras of a bake capture with its probes, enabled and at their baked intensity,
        // whatever the volume shows.
        _capture = _display;
        if (_captureTexture) {
            _capture.params[1] = 1.0f;
            _capture.blend[1] = 1.0f;
            _capture.encoding[0] = 0.0f;
            _capture.encoding[1] = 1.0f;
            _capture.lanes[0] = laneOf(_captureTexture.get());
            std::copy(std::begin(_bakeGeometry.bakeLanes), std::end(_bakeGeometry.bakeLanes), std::begin(_capture.bakeLanes));
            std::copy(std::begin(_bakeGeometry.bakeSkyLane), std::end(_bakeGeometry.bakeSkyLane),
                std::begin(_capture.bakeSkyLane));
            std::copy(std::begin(_bakeGeometry.bakeAtlas), std::end(_bakeGeometry.bakeAtlas), std::begin(_capture.bakeAtlas));
        }

        if (_device) {
            _device->setProbeVolumeBlocks(this, _display, _captureTexture ? &_capture : nullptr);
        }
    }
}
