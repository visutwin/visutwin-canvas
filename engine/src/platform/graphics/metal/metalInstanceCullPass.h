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

#include "platform/graphics/instanceCuller.h"

namespace visutwin::canvas
{
    class MetalGraphicsDevice;

    /**
     * GPU frustum culling for hardware-instanced draws via Metal compute kernels.
     *
     * Implements the backend-agnostic `InstanceCuller` interface. Owns compute
     * pipelines, counter/uniform/indirect-args buffers, and the compacted
     * instance output buffer. The compacted buffer is pre-allocated for a
     * maximum instance count and reused across frames.
     *
     * Thread-unsafe -- call from the main rendering thread only.
     */
    class MetalInstanceCullPass final : public InstanceCuller
    {
    public:
        explicit MetalInstanceCullPass(MetalGraphicsDevice* device);
        ~MetalInstanceCullPass() override;

        // Non-copyable
        MetalInstanceCullPass(const MetalInstanceCullPass&) = delete;
        MetalInstanceCullPass& operator=(const MetalInstanceCullPass&) = delete;

        // ── InstanceCuller interface ──────────────────────────────────
        void reserve(uint32_t maxInstances) override;
        void cull(VertexBuffer* input, const InstanceCullParams& params) override;
        [[nodiscard]] void* compactedNativeBuffer() const override { return compactedBuffer_; }
        [[nodiscard]] void* indirectArgsNativeBuffer() const override { return indirectArgsBuffer_; }
        [[nodiscard]] uint32_t maxInstances() const override { return maxInstances_; }
        [[nodiscard]] uint32_t visibleCountReadback() const override;

        // ── Metal-specific accessors (used by the render loop) ───────
        [[nodiscard]] MTL::Buffer* compactedBuffer() const { return compactedBuffer_; }
        [[nodiscard]] MTL::Buffer* indirectArgsBuffer() const { return indirectArgsBuffer_; }

        /// Direct MTL::Buffer cull entry point.
        /// The InstanceCuller::cull(VertexBuffer*, ...) override downcasts the
        /// VertexBuffer to MetalVertexBuffer and forwards here.
        void cullRaw(MTL::Buffer* inputBuffer, const InstanceCullParams& params);

        /// True once ensureResources() has succeeded.
        [[nodiscard]] bool isReady() const { return resourcesReady_; }

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
