// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.10.2025
//
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sceneMapHandles.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Shader;
    class Texture;
    class VertexBuffer;

    /// Where a compute kernel finds the scene maps attached with Compute::setSceneDepthMap /
    /// setSceneColorMap: FIXED slots outside the name-ordered table above, so the module that
    /// reads them (engine/shaders/slang/modules/vtscenemaps.slang, which declares the same
    /// numbers) works in any kernel whatever its own parameters are. Metal: the block at
    /// buffer 30, the depth at texture 30, the colour at texture 31 and its linear clamp
    /// sampler at sampler 15. Vulkan: descriptor SET 1, bindings 0-3 in that order (depth,
    /// colour, sampler, block); a compute without scene maps has no set 1.
    inline constexpr uint32_t kComputeSceneMapsMetalBufferSlot = 30u;
    inline constexpr uint32_t kComputeSceneDepthMetalTextureSlot = 30u;
    inline constexpr uint32_t kComputeSceneColorMetalTextureSlot = 31u;
    inline constexpr uint32_t kComputeSceneColorMetalSamplerSlot = 15u;
    inline constexpr uint32_t kComputeSceneMapsVulkanSet = 1u;

    /// The uniform block the scene-map module reads (`SceneMapsData` there), 112 bytes.
    struct ComputeSceneMapsBlock
    {
        float depthCameraParams[4];             // 1 / far, far, near, orthographic
        float depthViewProjectionInverse[16];   // column-major, GL clip (NDC z in [-1, 1])
        uint32_t depthViewport[4];              // x, y of the first texel, width, height
        // x: the depth is linear, y: it is the reciprocal of the linear depth, z: the colour is
        // gamma-encoded. DEVIATION: upstream compiles a shader variant per combination; the
        // kernels here are compiled once (no variant compile from a Compute), so the module
        // tests these uniform flags.
        uint32_t flags[4];
    };
    static_assert(sizeof(ComputeSceneMapsBlock) == 112, "ComputeSceneMapsBlock must match SceneMapsData");

    /**
     * A representation of a compute shader with the associated resources, that can be executed on the
     * GPU.
     *
     * Three parameter kinds are supported:
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

        /// Prepare the dispatch as a grid of workgroups. To dispatch for a number of threads
        /// instead, with the workgroups derived from them, use setupDispatchThreads.
        void setupDispatch(uint32_t x, uint32_t y, uint32_t z);
        /// Prepare the dispatch to take its workgroup counts from the GPU: the x, y and z
        /// written at `slot` of the device's indirect dispatch buffer
        /// (GraphicsDevice::getIndirectDispatchSlot), or of `buffer` when one is given, by an
        /// earlier kernel. A setupDispatch or setupDispatchThreads makes it direct again. The
        /// slot is good for the frame it was taken in.
        void setupIndirectDispatch(int slot, std::shared_ptr<VertexBuffer> buffer = nullptr);
        /// The indirect slot, -1 for a direct dispatch.
        int indirectSlot() const { return _indirectSlot; }
        /// The buffer the indirect arguments are read from: the one given, or the device's.
        std::shared_ptr<VertexBuffer> indirectBuffer() const;
        uint32_t dispatchX() const { return _dispatchX; }
        uint32_t dispatchY() const { return _dispatchY; }
        uint32_t dispatchZ() const { return _dispatchZ; }

        /// Prepare the dispatch for a number of THREADS in each dimension, a thread being one
        /// invocation of the kernel (its SV_DispatchThreadID). The workgroups are derived from
        /// the workgroup size (threadgroupSize below) and rounded up to whole workgroups, so
        /// the kernel skips the threads past the requested number. A dispatch in one dimension
        /// that needs more workgroups than the device allows in one
        /// (GraphicsDevice::maxComputeWorkgroupsPerDimension) is spread over rows of
        /// workgroups; the kernel then indexes its threads with the `vtdispatch` module's
        /// dispatchThreadInRange / dispatchThreadIndex, which account for that.
        ///
        /// DEVIATION: upstream assigns the thread count and the row width to uniforms the
        /// shader's chunk declares. Here the uniform block is the caller's (the class
        /// comment), so the caller writes dispatchThreads() into its own block, as one uint4,
        /// after this call, for the kernel to pass to those functions.
        void setupDispatchThreads(uint32_t x, uint32_t y = 1u, uint32_t z = 1u);
        /// The threads the last setupDispatchThreads was set up for (1, 1, 1 after a
        /// setupDispatch).
        const std::array<uint32_t, 3>& dispatchThreadCount() const { return _dispatchThreadCount; }
        /// The threads in a row of that dispatch: the x of the thread count, or the threads in a
        /// row of workgroups when a one-dimensional dispatch was spread over rows.
        uint32_t dispatchRowThreads() const { return _dispatchRowThreads; }
        /// Both, as the uint4 the `vtdispatch` functions take: the thread count, then the
        /// threads in a row.
        std::array<uint32_t, 4> dispatchThreads() const
        {
            return {_dispatchThreadCount[0], _dispatchThreadCount[1], _dispatchThreadCount[2], _dispatchRowThreads};
        }

        /// Threads per workgroup: the size the shader DECLARES (`[numthreads]` in Slang,
        /// `local_size_*` in GLSL, Shader::computeThreadGroupSize) whenever it declares one,
        /// and otherwise the size set here, which Metal's dispatch supplies to a custom MSL
        /// kernel (an MSL kernel declares none). Defaults to 8x8x1. A size set here that
        /// disagrees with the shader's is ignored, with a warning.
        void setThreadgroupSize(uint32_t x, uint32_t y, uint32_t z);
        uint32_t threadgroupSizeX() const { return threadgroupSize()[0]; }
        uint32_t threadgroupSizeY() const { return threadgroupSize()[1]; }
        uint32_t threadgroupSizeZ() const { return threadgroupSize()[2]; }
        std::array<uint32_t, 3> threadgroupSize() const;

        /// Workgroups needed for `count` of them in one dimension, spread as a near-square
        /// grid when they exceed `maxDimension`: {count, 1} when they fit, otherwise the
        /// fewest rows that do, with each row as short as they allow.
        static std::array<uint32_t, 2> calcDispatchSize(uint32_t count, uint32_t maxDimension);

        /// Attaches a camera's scene depth map (CameraComponent::sceneDepthMapHandle), read by a
        /// kernel that imports `vtscenemaps`: sceneDepthSize, sceneDepthViewport,
        /// sceneDepthNearClip / FarClip, sceneDepthLinear(texel) and
        /// sceneDepthWorldPosition(texel). Kept and read at EVERY dispatch, so this is called
        /// once: a dispatch reads the map the camera rendered most recently (dispatched before
        /// the camera renders, from an update, that is last frame's), decoded with the camera
        /// parameters it was rendered with. Null detaches it.
        void setSceneDepthMap(std::shared_ptr<SceneDepthMapHandle> handle) { _sceneDepthMap = std::move(handle); }

        /// Attaches a camera's scene colour map (CameraComponent::sceneColorMapHandle):
        /// sceneColorSize(lod), sceneColorLoad(texel, lod), sceneColorSample(uv, lod),
        /// sceneColorToLinear and sceneColorToDisplay. The colours come as the camera stored
        /// them, linear under a camera frame and gamma-encoded otherwise. Null detaches it.
        void setSceneColorMap(std::shared_ptr<SceneColorMapHandle> handle) { _sceneColorMap = std::move(handle); }

        const std::shared_ptr<SceneDepthMapHandle>& sceneDepthMap() const { return _sceneDepthMap; }
        const std::shared_ptr<SceneColorMapHandle>& sceneColorMap() const { return _sceneColorMap; }
        bool hasSceneMaps() const { return _sceneDepthMap || _sceneColorMap; }

        /// Called by the backend as it records the dispatch, the point in the frame the kernel
        /// runs at: reads the attached handles into the textures and block below. A map the
        /// camera has not rendered (or whose texture is gone) is a substitute: a 1x1 depth of
        /// 1 (the far plane) or a pink colour, as upstream binds; a missing one is reported once.
        void prepareDispatch();

        /// What prepareDispatch resolved; valid while hasSceneMaps().
        Texture* sceneDepthTexture() const { return _boundSceneDepth.get(); }
        Texture* sceneColorTexture() const { return _boundSceneColor.get(); }
        const ComputeSceneMapsBlock& sceneMapsBlock() const { return _sceneMapsBlock; }

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
        int _indirectSlot = -1;
        std::shared_ptr<VertexBuffer> _indirectBuffer;
        uint32_t _dispatchX = 1u;
        uint32_t _dispatchY = 1u;
        uint32_t _dispatchZ = 1u;
        uint32_t _threadgroupX = 8u;
        uint32_t _threadgroupY = 8u;
        uint32_t _threadgroupZ = 1u;
        std::array<uint32_t, 3> _dispatchThreadCount{1u, 1u, 1u};
        uint32_t _dispatchRowThreads = 1u;
        bool _warnedThreadgroupMismatch = false;
        bool _warnedExtraDimensions = false;

        std::shared_ptr<SceneDepthMapHandle> _sceneDepthMap;
        std::shared_ptr<SceneColorMapHandle> _sceneColorMap;
        // Held for the dispatch: a texture locked from a handle, or a substitute.
        std::shared_ptr<Texture> _boundSceneDepth;
        std::shared_ptr<Texture> _boundSceneColor;
        std::shared_ptr<Texture> _depthSubstitute;
        std::shared_ptr<Texture> _colorSubstitute;
        ComputeSceneMapsBlock _sceneMapsBlock{};
        bool _reportedMissingDepth = false;
        bool _reportedMissingColor = false;
    };
}
