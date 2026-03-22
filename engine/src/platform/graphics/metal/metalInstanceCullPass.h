// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Metal compute pass for GPU frustum culling of instanced draws.
//
// Two-kernel pipeline:
//   Kernel 1 (instanceCull): Each thread tests one instance's bounding sphere
//     against 6 frustum planes. Visible instances are compacted into an output
//     buffer via atomic_fetch_add.
//   Kernel 2 (writeIndirectArgs): A single thread reads the atomic counter and
//     writes MTLDrawIndexedPrimitivesIndirectArguments (20 bytes).
//
// Follows the MetalMarchingCubesPass pattern:
//   - Embedded MSL source as string literal
//   - Lazy resource creation
//   - Friend access to MetalGraphicsDevice for command queue
//
// Custom shader -- no upstream GLSL equivalent exists for GPU instance culling.
// Upstream GPU culling is GSplat-specific; this is a general-purpose extension.
//
#pragma once

#include <cstdint>
#include <Metal/Metal.hpp>

namespace visutwin::canvas
{
    class MetalGraphicsDevice;

    /// Parameters for GPU instance culling.
    /// Must match the CullParams struct in the embedded MSL.
    struct alignas(16) InstanceCullParams
    {
        float frustumPlanes[6][4];  ///< 6 planes: (nx, ny, nz, d). dot(n,p)+d >= 0 = inside.
        float boundingSphereRadius; ///< Bounding sphere radius for each instance.
        uint32_t instanceCount;     ///< Total input instances.
        uint32_t indexCount;        ///< Mesh Primitive.count -> indirect args.
        uint32_t indexStart;        ///< Mesh Primitive.base -> indirect args.
        int32_t  baseVertex;        ///< Mesh Primitive.baseVertex -> indirect args.
        uint32_t baseInstance;      ///< Always 0.
        float    _pad[2];
    };
    static_assert(sizeof(InstanceCullParams) == 128,
        "InstanceCullParams must be 128 bytes to match MSL layout");

    /**
     * GPU frustum culling for hardware-instanced draws via Metal compute kernels.
     *
     * Owns compute pipelines, counter/uniform/indirect-args buffers, and the
     * compacted instance output buffer. The compacted buffer is pre-allocated
     * for a maximum instance count and reused across frames.
     *
     * Thread-unsafe -- call from the main rendering thread only.
     */
    class MetalInstanceCullPass
    {
    public:
        explicit MetalInstanceCullPass(MetalGraphicsDevice* device);
        ~MetalInstanceCullPass();

        // Non-copyable
        MetalInstanceCullPass(const MetalInstanceCullPass&) = delete;
        MetalInstanceCullPass& operator=(const MetalInstanceCullPass&) = delete;

        /// Ensure the compacted buffer can hold at least maxInstances.
        /// Re-allocates if the current capacity is smaller. Safe to call every frame.
        void reserve(uint32_t maxInstances);

        /// Run frustum culling on the input instance buffer.
        /// After this call, compactedBuffer() contains only visible instances
        /// and indirectArgsBuffer() contains the draw arguments.
        ///
        /// @param inputBuffer  MTL::Buffer with packed InstanceData (80 bytes each).
        /// @param params       Culling parameters (frustum planes, counts, etc.).
        void cull(MTL::Buffer* inputBuffer, const InstanceCullParams& params);

        /// The compacted output buffer (visible instances only, 80 bytes each).
        /// Valid after a successful cull() call.
        [[nodiscard]] MTL::Buffer* compactedBuffer() const { return compactedBuffer_; }

        /// The indirect draw arguments buffer (20 bytes: MTLDrawIndexedPrimitivesIndirectArguments).
        /// Valid after a successful cull() call.
        [[nodiscard]] MTL::Buffer* indirectArgsBuffer() const { return indirectArgsBuffer_; }

        /// True once ensureResources() has succeeded.
        [[nodiscard]] bool isReady() const { return resourcesReady_; }

        /// Extract 6 frustum planes from a view-projection matrix (Gribb/Hartmann method).
        /// Each plane is (nx, ny, nz, d) with the convention: dot(n,p)+d >= 0 = inside.
        static void extractFrustumPlanes(const float* vpMatrix4x4ColMajor, float outPlanes[6][4]);

    private:
        void ensureResources();

        MetalGraphicsDevice* device_;

        // Compute pipelines
        MTL::ComputePipelineState* cullPipeline_      = nullptr;
        MTL::ComputePipelineState* writeArgsPipeline_  = nullptr;

        // Buffers (reused across frames)
        MTL::Buffer* compactedBuffer_    = nullptr;  // Visible instances (maxInstances * 80)
        MTL::Buffer* indirectArgsBuffer_ = nullptr;  // 20 bytes (MTLDrawIndexedPrimitivesIndirectArguments)
        MTL::Buffer* counterBuffer_      = nullptr;  // atomic_uint (4 bytes)
        MTL::Buffer* uniformBuffer_      = nullptr;  // InstanceCullParams (128 bytes)

        uint32_t maxInstances_ = 0;

        bool resourcesReady_ = false;
    };

} // namespace visutwin::canvas
