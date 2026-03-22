// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.09.2025.
//
#pragma once

#include <memory>
#include <string>

namespace visutwin::canvas
{
    struct ShaderDefinition
    {
        std::string name;

        std::string vshader;  // Vertex shader entry-point name
        std::string fshader;  // Fragment shader entry-point name
        std::string cshader;  // Compute shader entry-point name (optional)
    };

    class GraphicsDevice;

    /**
     * A shader is a program that is responsible for rendering graphical primitives on a device's
     * graphics processor. The shader is generated from a shader definition. This shader definition
     * specifies the code for processing vertices and fragments processed by the GPU.
     */
    class Shader
    {
    public:
        Shader(GraphicsDevice* graphicsDevice, const ShaderDefinition& definition);
        virtual ~Shader() = default;

        int id() const { return _id; }
        const std::string& vertexEntry() const { return _definition.vshader; }
        const std::string& fragmentEntry() const { return _definition.fshader; }
        const std::string& computeEntry() const { return _definition.cshader; }
        GraphicsDevice* graphicsDevice() const { return _device; }

    private:
        // Process shader definition and create implementation
        void processDefinition();

        // Process vertex and fragment shaders
        void processVertexFragmentShaders();

        // Check platform compatibility
        void validatePlatformSupport() const;

        static int _nextId;

        GraphicsDevice* _device;
        int _id;
        ShaderDefinition _definition;
    };

    std::shared_ptr<Shader> createShader(GraphicsDevice* graphicsDevice, const ShaderDefinition& definition,
        const std::string& sourceCode = "");
}
