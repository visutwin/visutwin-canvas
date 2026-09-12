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
     * from the reflected names. This port has no shader reflection, so indices are derived from
     * the parameter NAMES in sorted order and the shader must declare them to match. Within each
     * kind the order is the same on both backends — buffers name-sorted, then textures
     * name-sorted, then the single uniform block — but the INDICES ARE NOT, because Metal has a
     * separate index namespace per resource kind and Vulkan has one flat descriptor set:
     *
     *   resource                MSL                        GLSL (descriptor set 0)
     *   ----------------------  -------------------------  ------------------------
     *   buffers (b of them)     buffer(0 .. b-1)           binding 0 .. b-1
     *   uniform block           buffer(b), via setBytes    binding b+t
     *   textures (t of them)    texture(0 .. t-1)          binding b .. b+t-1
     *
     * So a kernel with BOTH buffers and textures cannot use the same indices in the two
     * languages: on Metal its first texture is texture(0) and the uniform block sits directly
     * after the buffers, while on Vulkan the textures follow the buffers and the uniform block
     * comes last. Nothing in the tree exercises that combination yet — the one shipped kernel,
     * the particle simulation, is one buffer plus one uniform block and no textures, which is
     * exactly the case where the two layouts agree — so treat the table, not any existing
     * kernel, as the contract.
     *
     * The uniform block is ONE block whose members are the scalar parameters packed 4 bytes
     * each, again in name-sorted order, or the bytes handed to setUniformBlock verbatim. It is
     * bound only when non-empty; because it is last in each namespace, leaving it out shifts
     * nothing else. The same is true of a texture-only or buffer-only compute, which keeps the
     * indices it had before the other kinds existed.
     *
     * Textures: Vulkan picks a storage image or a combined image sampler from Texture::storage(),
     * and a combined image sampler carries the texture's own sampler. The Metal path binds
     * textures alone and NO sampler state at all, so an MSL kernel that wants filtered sampling
     * has to declare a constexpr sampler of its own.
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

        /// Supply the uniform block VERBATIM instead of building it from named
        /// scalars. The block still lands where the class comment's table says —
        /// buffer(b) on Metal, binding b+t on Vulkan — so a kernel declares it the
        /// same way; this only changes how the bytes are produced. Use it when the
        /// parameters are a struct rather than
        /// a handful of scalars: expressing a mat4 plus seven vec4s as 44 separately
        /// named floats would be unreadable AND fragile, because the loose path
        /// orders members by NAME.
        ///
        /// Mutually exclusive with the scalar setters; whichever is non-empty wins,
        /// and setting a scalar clears the block.
        void setUniformBlock(const void* data, size_t size);

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
        std::map<std::string, uint32_t> _uniformParameters;
        // Set by setUniformBlock; takes precedence over _uniformParameters.
        std::vector<uint8_t> _uniformBlock;
        uint32_t _dispatchX = 1u;
        uint32_t _dispatchY = 1u;
        uint32_t _dispatchZ = 1u;
        uint32_t _threadgroupX = 8u;
        uint32_t _threadgroupY = 8u;
        uint32_t _threadgroupZ = 1u;
    };
}
