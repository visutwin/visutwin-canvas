// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis  on 19.10.2025.
//

#include "compute.h"

#include <algorithm>
#include <cstring>
#include <ranges>
#include <utility>

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
        _bufferParameters[name] = buffer;
    }

    void Compute::setParameter(const std::string& name, const float value)
    {
        uint32_t bits = 0u;
        std::memcpy(&bits, &value, sizeof(bits));
        _uniformParameters[name] = bits;
    }

    void Compute::setParameter(const std::string& name, const uint32_t value)
    {
        _uniformParameters[name] = value;
    }

    std::vector<uint8_t> Compute::uniformData() const
    {
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
        _dispatchX = std::max(1u, x);
        _dispatchY = std::max(1u, y);
        _dispatchZ = std::max(1u, z);
    }

    void Compute::setThreadgroupSize(uint32_t x, uint32_t y, uint32_t z)
    {
        _threadgroupX = std::max(1u, x);
        _threadgroupY = std::max(1u, y);
        _threadgroupZ = std::max(1u, z);
    }
} // visutwin
