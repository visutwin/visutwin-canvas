// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.10.2025
//

#include "compute.h"

#include <algorithm>
#include <cstring>
#include <ranges>
#include <utility>

#include "graphicsDevice.h"
#include "shader.h"
#include "spdlog/spdlog.h"
#include "texture.h"
#include "vertexBuffer.h"

namespace visutwin::canvas
{
    Compute::Compute(GraphicsDevice* graphicsDevice, const std::shared_ptr<Shader>& shader, std::string name)
        : _graphicsDevice(graphicsDevice), _shader(shader), _name(std::move(name))
    {
    }

    void Compute::setParameter(const std::string& name, Texture* texture)
    {
        _textureParameters[name] = texture;
    }

    Texture* Compute::getTextureParameter(const std::string& name) const
    {
        const auto it = _textureParameters.find(name);
        return it != _textureParameters.end() ? it->second : nullptr;
    }

    void Compute::setParameter(const std::string& name, const std::shared_ptr<VertexBuffer>& buffer)
    {
        if (buffer) {
            buffer->markStorageUse();
        }
        _bufferParameters[name] = buffer;
    }

    void Compute::setParameter(const std::string& name, const float value)
    {
        _uniformBlock.clear();
        uint32_t bits = 0u;
        std::memcpy(&bits, &value, sizeof(bits));
        _uniformParameters[name] = bits;
    }

    void Compute::setParameter(const std::string& name, const uint32_t value)
    {
        _uniformBlock.clear();
        _uniformParameters[name] = value;
    }

    void Compute::setUniformBlock(const void* data, const size_t size)
    {
        _uniformParameters.clear();
        const auto* bytes = static_cast<const uint8_t*>(data);
        _uniformBlock.assign(bytes, bytes + size);
    }

    std::vector<uint8_t> Compute::uniformData() const
    {
        if (!_uniformBlock.empty()) {
            return _uniformBlock;
        }
        std::vector<uint8_t> data(_uniformParameters.size() * sizeof(uint32_t));
        size_t offset = 0;
        for (const auto& bits : _uniformParameters | std::views::values) {
            std::memcpy(data.data() + offset, &bits, sizeof(bits));
            offset += sizeof(bits);
        }
        return data;
    }

    void Compute::setupDispatch(uint32_t x, uint32_t y, uint32_t z)
    {
        _indirectSlot = -1;
        _indirectBuffer.reset();
        _dispatchX = std::max(1u, x);
        _dispatchY = std::max(1u, y);
        _dispatchZ = std::max(1u, z);
        _dispatchThreadCount = {1u, 1u, 1u};
        _dispatchRowThreads = 1u;
    }

    void Compute::setupIndirectDispatch(const int slot, std::shared_ptr<VertexBuffer> buffer)
    {
        setupDispatch(1u, 1u, 1u);
        _indirectSlot = slot;
        _indirectBuffer = std::move(buffer);
    }

    std::shared_ptr<VertexBuffer> Compute::indirectBuffer() const
    {
        if (_indirectBuffer) {
            return _indirectBuffer;
        }
        return _graphicsDevice ? _graphicsDevice->indirectDispatchBuffer() : nullptr;
    }

    void Compute::setupDispatchThreads(const uint32_t x, const uint32_t y, const uint32_t z)
    {
        const auto [sizeX, sizeY, sizeZ] = threadgroupSize();
        const auto groups = [](const uint32_t threads, const uint32_t size) {
            return static_cast<uint32_t>((static_cast<uint64_t>(threads) + size - 1u) / size);
        };
        uint32_t countX = groups(x, sizeX);
        uint32_t countY = groups(y, sizeY);
        const uint32_t countZ = groups(z, sizeZ);
        uint32_t rowThreads = x;

        // A dispatch in one dimension that needs more workgroups than one dimension holds
        // is spread over rows of them.
        const uint32_t maxDimension = _graphicsDevice ? _graphicsDevice->maxComputeWorkgroupsPerDimension() : 65535u;
        if (countX > maxDimension && y == 1u && z == 1u && sizeY == 1u && sizeZ == 1u) {
            const auto spread = calcDispatchSize(countX, maxDimension);
            countX = spread[0];
            countY = spread[1];
            rowThreads = countX * sizeX;
        }
        if (countX > maxDimension || countY > maxDimension || countZ > maxDimension) {
            spdlog::error("Compute {}: the dispatch of [{}, {}, {}] threads needs [{}, {}, {}] workgroups, more than "
                "the {} the device supports in a dimension", _name, x, y, z, countX, countY, countZ, maxDimension);
        }
        if (!_warnedExtraDimensions && ((y == 1u && sizeY > 1u) || (z == 1u && sizeZ > 1u))) {
            _warnedExtraDimensions = true;
            spdlog::warn("Compute {}: the workgroup size [{}, {}, {}] has more dimensions than the dispatch of "
                "[{}, {}, {}] threads, which leaves most of its threads without work", _name, sizeX, sizeY, sizeZ,
                x, y, z);
        }

        setupDispatch(countX, countY, countZ);
        _dispatchThreadCount = {x, y, z};
        _dispatchRowThreads = rowThreads;
    }

    std::array<uint32_t, 2> Compute::calcDispatchSize(const uint32_t count, const uint32_t maxDimension)
    {
        const uint32_t limit = std::max(1u, maxDimension);
        if (count <= limit) {
            return {count, 1u};
        }
        const uint32_t rows = (count + limit - 1u) / limit;
        return {(count + rows - 1u) / rows, rows};
    }

    void Compute::setThreadgroupSize(uint32_t x, uint32_t y, uint32_t z)
    {
        _threadgroupX = std::max(1u, x);
        _threadgroupY = std::max(1u, y);
        _threadgroupZ = std::max(1u, z);
        if (_shader && _shader->hasComputeThreadGroupSize() && !_warnedThreadgroupMismatch &&
            _shader->computeThreadGroupSize() != std::array<uint32_t, 3>{_threadgroupX, _threadgroupY, _threadgroupZ}) {
            _warnedThreadgroupMismatch = true;
            const auto& declared = _shader->computeThreadGroupSize();
            spdlog::warn("Compute {}: threadgroup size [{}, {}, {}] ignored; the shader declares [{}, {}, {}]", _name,
                _threadgroupX, _threadgroupY, _threadgroupZ, declared[0], declared[1], declared[2]);
        }
    }

    std::array<uint32_t, 3> Compute::threadgroupSize() const
    {
        if (_shader && _shader->hasComputeThreadGroupSize()) {
            return _shader->computeThreadGroupSize();
        }
        return {_threadgroupX, _threadgroupY, _threadgroupZ};
    }

    namespace
    {
        /// A 1x1 texture holding one texel, the stand-in bound for a scene map that is not there.
        std::shared_ptr<Texture> createSubstitute(GraphicsDevice* device, const PixelFormat format,
            const char* name, const void* texel, const size_t texelBytes)
        {
            TextureOptions options;
            options.name = name;
            options.width = 1;
            options.height = 1;
            options.format = format;
            options.mipmaps = false;
            auto texture = std::make_shared<Texture>(device, options);
            texture->setLevelData(0, static_cast<const uint8_t*>(texel), texelBytes);
            texture->upload();
            return texture;
        }

        /// True for a handle's texture that was never published, as opposed to one published
        /// and since freed (a weak pointer that never owned anything has no owner block).
        bool neverPublished(const std::weak_ptr<Texture>& texture)
        {
            const std::weak_ptr<Texture> empty;
            return !texture.owner_before(empty) && !empty.owner_before(texture);
        }
    }

    void Compute::prepareDispatch()
    {
        // What the last dispatch held is let go first, so a texture its producer has freed
        // since reads as freed rather than being kept alive here.
        _boundSceneDepth.reset();
        _boundSceneColor.reset();
        if (const auto& handle = _sceneDepthMap) {
            _boundSceneDepth = handle->texture.lock();
            if (!_boundSceneDepth) {
                // Not rendered, a texture its producer has since freed (an option change that
                // rebuilt the camera's targets, expected until the next publication), or a
                // destroyed camera: the far plane everywhere.
                if (!handle->destroyed && neverPublished(handle->texture) && !_reportedMissingDepth) {
                    _reportedMissingDepth = true;
                    spdlog::warn("Compute {}: the camera of the attached scene depth map has not rendered it. A "
                        "camera renders it only when requested (CameraComponent::requestSceneDepthMap) or when "
                        "its camera frame needs the depth.", _name);
                }
                if (!_depthSubstitute) {
                    constexpr float far = 1.0f;
                    _depthSubstitute = createSubstitute(_graphicsDevice, PixelFormat::PIXELFORMAT_R32F,
                        "computeSceneDepthSubstitute", &far, sizeof(far));
                }
                _boundSceneDepth = _depthSubstitute;
            }
            auto& block = _sceneMapsBlock;
            std::copy_n(handle->cameraParams.begin(), 4, block.depthCameraParams);
            handle->viewProjectionInverse.store(block.depthViewProjectionInverse);
            std::copy_n(handle->viewport.begin(), 4, block.depthViewport);
            if (_boundSceneDepth == _depthSubstitute) {
                block.depthViewport[0] = block.depthViewport[1] = 0u;
                block.depthViewport[2] = block.depthViewport[3] = 1u;
            }
            block.flags[0] = handle->linear ? 1u : 0u;
            block.flags[1] = handle->reciprocal ? 1u : 0u;
        } else {
            _boundSceneDepth.reset();
        }

        if (const auto& handle = _sceneColorMap) {
            _boundSceneColor = handle->texture.lock();
            if (!_boundSceneColor) {
                if (!handle->destroyed && neverPublished(handle->texture) && !_reportedMissingColor) {
                    _reportedMissingColor = true;
                    spdlog::warn("Compute {}: the camera of the attached scene color map has not rendered it. A "
                        "camera renders it only when requested (CameraComponent::requestSceneColorMap) or when "
                        "its camera frame is configured to.", _name);
                }
                if (!_colorSubstitute) {
                    constexpr uint8_t pink[4] = {255u, 0u, 255u, 255u};
                    _colorSubstitute = createSubstitute(_graphicsDevice, PixelFormat::PIXELFORMAT_RGBA8,
                        "computeSceneColorSubstitute", pink, sizeof(pink));
                }
                _boundSceneColor = _colorSubstitute;
            }
            _sceneMapsBlock.flags[2] = handle->gamma ? 1u : 0u;
        } else {
            _boundSceneColor.reset();
        }
    }
} // visutwin
