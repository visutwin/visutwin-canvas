// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.10.2025.
//
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Shader;
    class Texture;

    /**
     * A representation of a compute shader with the associated resources, that can be executed on the
     * GPU.
     */
    class Compute
    {
    public:
        Compute(GraphicsDevice* graphicsDevice, const std::shared_ptr<Shader>& shader, std::string name = "");

        const std::shared_ptr<Shader>& shader() const { return _shader; }
        GraphicsDevice* graphicsDevice() const { return _graphicsDevice; }
        const std::string& name() const { return _name; }

        void setParameter(const std::string& name, Texture* texture);
        Texture* getTextureParameter(const std::string& name) const;
        const std::unordered_map<std::string, Texture*>& textureParameters() const { return _textureParameters; }

        // Matches upstream setupDispatch() semantics: workgroup group counts.
        void setupDispatch(uint32_t x, uint32_t y, uint32_t z);
        uint32_t dispatchX() const { return _dispatchX; }
        uint32_t dispatchY() const { return _dispatchY; }
        uint32_t dispatchZ() const { return _dispatchZ; }

    private:
        GraphicsDevice* _graphicsDevice = nullptr;
        std::shared_ptr<Shader> _shader = nullptr;
        std::string _name;
        std::unordered_map<std::string, Texture*> _textureParameters;
        uint32_t _dispatchX = 1u;
        uint32_t _dispatchY = 1u;
        uint32_t _dispatchZ = 1u;
    };
}
