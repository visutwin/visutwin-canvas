// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.10.2025.
//
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Shader;
    class Texture;
    class VertexBuffer;

    /**
     * A representation of a compute shader with the associated resources, that can be executed on the
     * GPU.
     *
     * Three parameter kinds are supported, mirroring upstream's simplified compute syntax:
     * storage buffers, textures, and loose scalar uniforms.
     *
     * DEVIATION: upstream reflects the resources out of the WGSL source and builds the bind group
     * from the reflected names. This port has no shader reflection, so binding indices are derived
     * from the parameter NAMES in sorted order, and the shader must declare them to match:
     *
     *   buffers  (name-sorted)  -> binding 0 .. b-1
     *   textures (name-sorted)  -> binding b .. b+t-1
     *   loose uniforms          -> binding b+t, as ONE block whose members are the scalar
     *                              parameters packed 4 bytes each, again in name-sorted order
     *
     * On Metal those indices are buffer(n) / texture(n) slots (the uniform block arrives via
     * setBytes); on Vulkan they are bindings in descriptor set 0. A texture-only compute keeps
     * the indices it had before buffers and uniforms existed.
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

        /// Binds a storage buffer the kernel reads and/or writes. The buffer is the engine's
        /// generic GPU storage vehicle (VertexBuffer), so the same object can be bound to a draw.
        void setParameter(const std::string& name, const std::shared_ptr<VertexBuffer>& buffer);
        const std::map<std::string, std::shared_ptr<VertexBuffer>>& bufferParameters() const
        {
            return _bufferParameters;
        }

        /// Loose scalar uniforms. Collapsed into a single uniform block — see the class comment
        /// for the member order the shader must declare.
        void setParameter(const std::string& name, float value);
        void setParameter(const std::string& name, uint32_t value);

        /// The packed uniform block, empty when no scalar parameters were set.
        std::vector<uint8_t> uniformData() const;

        // Matches upstream setupDispatch() semantics: workgroup group counts.
        void setupDispatch(uint32_t x, uint32_t y, uint32_t z);
        uint32_t dispatchX() const { return _dispatchX; }
        uint32_t dispatchY() const { return _dispatchY; }
        uint32_t dispatchZ() const { return _dispatchZ; }

        /// Threads per workgroup — must match the shader's declared workgroup size
        /// (`[[threads_per_threadgroup]]` dispatch on Metal, `local_size_*` in GLSL).
        /// Defaults to 8x8x1, the size every compute kernel used before this was configurable.
        void setThreadgroupSize(uint32_t x, uint32_t y, uint32_t z);
        uint32_t threadgroupSizeX() const { return _threadgroupX; }
        uint32_t threadgroupSizeY() const { return _threadgroupY; }
        uint32_t threadgroupSizeZ() const { return _threadgroupZ; }

    private:
        GraphicsDevice* _graphicsDevice = nullptr;
        std::shared_ptr<Shader> _shader = nullptr;
        std::string _name;
        std::unordered_map<std::string, Texture*> _textureParameters;
        // Ordered: the binding index of a buffer / uniform member is its position here.
        std::map<std::string, std::shared_ptr<VertexBuffer>> _bufferParameters;
        std::map<std::string, uint32_t> _uniformParameters;   // raw 4-byte payload, float bit-cast
        uint32_t _dispatchX = 1u;
        uint32_t _dispatchY = 1u;
        uint32_t _dispatchZ = 1u;
        uint32_t _threadgroupX = 8u;
        uint32_t _threadgroupY = 8u;
        uint32_t _threadgroupZ = 1u;
    };
}
