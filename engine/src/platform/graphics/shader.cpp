// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.09.2025.
//

#include "shader.h"

#include <assert.h>

#include "graphicsDevice.h"

namespace visutwin::canvas
{
    int Shader::_nextId = 0;

    Shader::Shader(GraphicsDevice* graphicsDevice, const ShaderDefinition& definition) :
        _device(graphicsDevice), _id(_nextId++), _definition(definition)
    {

    }

    void Shader::processDefinition()
    {

    }

    void Shader::processVertexFragmentShaders()
    {
        assert(!_definition.vshader.empty() && "No vertex shader has been specified when creating a shader.");
        assert(!_definition.fshader.empty() && "No fragment shader has been specified when creating a shader.");
    }

    void Shader::validatePlatformSupport() const
    {

    }

    std::shared_ptr<Shader> createShader(GraphicsDevice* graphicsDevice, const ShaderDefinition& definition,
        const std::string& sourceCode)
    {
        return graphicsDevice->createShader(definition, sourceCode);
    }
}
