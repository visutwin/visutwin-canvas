// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis  on 19.10.2025.
//

#include "compute.h"

#include <algorithm>
#include <utility>

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

    void Compute::setupDispatch(uint32_t x, uint32_t y, uint32_t z)
    {
        _dispatchX = std::max(1u, x);
        _dispatchY = std::max(1u, y);
        _dispatchZ = std::max(1u, z);
    }
} // visutwin
