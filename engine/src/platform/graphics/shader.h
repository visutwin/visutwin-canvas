// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.09.2025
//
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "platform/graphics/shaderFeatures.h"

namespace visutwin::canvas
{
    struct ShaderDefinition
    {
        std::string name;

        std::string vshader;  // Vertex shader entry-point name
        std::string fshader;  // Fragment shader entry-point name
        std::string cshader;  // Compute shader entry-point name (optional)
        // Backend-independent feature selection resolved by ProgramLibrary.
        // Metal emits these as defines; Vulkan uses specialization constants.
        ShaderFeatureSet features;
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
        const std::string& name() const { return _definition.name; }
        GraphicsDevice* graphicsDevice() const { return _device; }

    private:
        // Process shader definition and create implementation
        void processDefinition();

        // Process vertex and fragment shaders
        void processVertexFragmentShaders();

        // Check platform compatibility
        void validatePlatformSupport() const;

        static int _nextId;

        // This shader's place in its device's live shader count (GraphicsDevice::
        // liveResourceCounts). The counter is co-owned, so a shader that outlives its device
        // still decrements memory that exists. A copy counts as a shader of its own.
        class LiveCount
        {
        public:
            explicit LiveCount(std::shared_ptr<std::atomic<int>> counter) : _counter(std::move(counter)) { add(1); }
            LiveCount(const LiveCount& other) : LiveCount(other._counter) {}
            LiveCount& operator=(const LiveCount& other)
            {
                if (this != &other) {
                    add(-1);
                    _counter = other._counter;
                    add(1);
                }
                return *this;
            }
            ~LiveCount() { add(-1); }

        private:
            void add(const int delta) const
            {
                if (_counter) {
                    _counter->fetch_add(delta, std::memory_order_relaxed);
                }
            }
            std::shared_ptr<std::atomic<int>> _counter;
        };

        GraphicsDevice* _device;
        int _id;
        ShaderDefinition _definition;
        LiveCount _live;
    };

    std::shared_ptr<Shader> createShader(GraphicsDevice* graphicsDevice, const ShaderDefinition& definition,
        const std::string& sourceCode = "");
}
