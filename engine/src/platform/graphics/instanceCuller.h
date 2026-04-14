// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Backend-agnostic interface for GPU frustum culling of instanced draws.
//
// Concrete implementations (e.g. MetalInstanceCullPass) run a compute pipeline
// that tests each instance's bounding sphere against a camera frustum and
// writes the visible instances into a compacted buffer plus an indirect draw
// arguments buffer. The forward renderer then consumes those via indirect
// instancing (see renderer.cpp draw dispatch).
//
#pragma once

#include <cstdint>

namespace visutwin::canvas
{
    class VertexBuffer;

    /// Parameters for GPU instance culling.
    /// Layout must match the CullParams struct used inside backend compute kernels
    /// (for Metal, see the embedded MSL in metalInstanceCullPass.cpp).
    struct alignas(16) InstanceCullParams
    {
        float frustumPlanes[6][4];  ///< 6 planes: (nx, ny, nz, d). dot(n,p)+d >= 0 = inside.
        float boundingSphereRadius; ///< Bounding sphere radius shared by all instances.
        uint32_t instanceCount;     ///< Total input instances.
        uint32_t indexCount;        ///< Mesh Primitive.count -> indirect args.
        uint32_t indexStart;        ///< Mesh Primitive.base -> indirect args.
        int32_t  baseVertex;        ///< Mesh Primitive.baseVertex -> indirect args.
        uint32_t baseInstance;      ///< Always 0.
        float    _pad[2];
    };
    static_assert(sizeof(InstanceCullParams) == 128,
        "InstanceCullParams must be 128 bytes to match backend kernel layout");

    /**
     * @brief Backend-agnostic handle for per-frame GPU instance culling.
     * @ingroup group_platform_graphics
     *
     * A culler is owned by a single MeshInstance. It holds a compacted output
     * buffer (`compactedNativeBuffer()`) and an indirect-args buffer
     * (`indirectArgsNativeBuffer()`) that are overwritten each time `cull()` is
     * called. The MeshInstance wraps the compacted buffer as a VertexBuffer
     * once at setup and reuses that wrapper across frames, since the underlying
     * native buffer is stable after `reserve()`.
     *
     * Thread-unsafe — call from the main rendering thread only.
     */
    class InstanceCuller
    {
    public:
        virtual ~InstanceCuller() = default;

        /// Ensure the compacted buffer can hold at least `maxInstances`.
        /// Safe to call every frame; backends re-allocate only when the
        /// capacity needs to grow. Must be called at least once before
        /// `compactedNativeBuffer()` returns a valid pointer.
        virtual void reserve(uint32_t maxInstances) = 0;

        /// Run a frustum cull pass over `input` (packed `InstanceData`, 80 bytes each).
        /// After this call, `compactedNativeBuffer()` contains only the visible
        /// instances and `indirectArgsNativeBuffer()` contains the draw arguments.
        virtual void cull(VertexBuffer* input, const InstanceCullParams& params) = 0;

        /// Opaque pointer to the compacted instance buffer
        /// (backend-specific: `MTL::Buffer*`, `VkBuffer`, …). Stable across frames
        /// unless `reserve()` reallocates due to growth.
        [[nodiscard]] virtual void* compactedNativeBuffer() const = 0;

        /// Opaque pointer to the indirect draw arguments buffer
        /// (20 bytes: indexCount, instanceCount, indexStart, baseVertex, baseInstance).
        [[nodiscard]] virtual void* indirectArgsNativeBuffer() const = 0;

        /// Current reserved capacity (max instances the compacted buffer can hold).
        [[nodiscard]] virtual uint32_t maxInstances() const = 0;

        /// CPU read-back of the most recent visible instance count written by
        /// cull(). Requires the backend to keep the indirect-args buffer in
        /// host-visible memory (Metal default: ResourceStorageModeShared).
        /// Returns 0 if the buffer is not yet allocated. Intended for
        /// diagnostics / UI overlays — avoid calling every frame in hot paths.
        [[nodiscard]] virtual uint32_t visibleCountReadback() const = 0;

        /// Extract 6 frustum planes from a column-major view-projection matrix
        /// using the Gribb/Hartmann method. Each plane is (nx, ny, nz, d) with
        /// the convention `dot(n,p)+d >= 0 = inside`. Planes are normalized.
        static void extractFrustumPlanes(const float* vpColMajor, float outPlanes[6][4]);
    };

} // namespace visutwin::canvas
