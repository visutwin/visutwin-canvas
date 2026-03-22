// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Metal compute pass for GPU Marching Cubes isosurface extraction.
//
// Two-pass atomic pipeline:
//   Pass 1 (classifyCells): Each thread classifies one cell, computes its cube
//     index from the 3D volume texture, counts vertices via the triangle table,
//     and atomically adds to a global vertex counter.
//   Pass 2 (generateVertices): Each thread reprocesses its cell, atomically
//     allocates space in the output buffer, and writes full 56-byte VertexData
//     (position, normal, UV, tangent) matching the engine's common.metal layout.
//
// Follows the MetalParticleComputePass pattern:
//   - Embedded MSL source as string literal
//   - Lazy resource creation
//   - Friend access to MetalGraphicsDevice for command queue
//
// Custom shader -- no upstream GLSL equivalent exists for GPU
// Marching Cubes isosurface extraction.
//
#pragma once

#include <cstdint>
#include <vector>
#include <Metal/Metal.hpp>

namespace visutwin::canvas
{
    class MetalGraphicsDevice;
    class Texture;

    /// Parameters for GPU Marching Cubes extraction.
    /// Must match the MCParams struct in the embedded MSL.
    struct alignas(16) MCComputeParams
    {
        uint32_t dimsX, dimsY, dimsZ;   ///< Volume dimensions in voxels.
        float    isovalue;                ///< Isosurface threshold.
        float    domainMinX, domainMinY, domainMinZ;
        float    _pad0;
        float    domainMaxX, domainMaxY, domainMaxZ;
        float    _pad1;
        float    texelSizeX, texelSizeY, texelSizeZ; ///< 1.0 / (dims - 1)
        uint32_t maxVertices;             ///< Safety cap for output buffer.
        uint32_t flipNormals;             ///< 0 = outward (low-to-high), 1 = inward.
        float    _pad2[3];
    };
    static_assert(sizeof(MCComputeParams) == 80,
        "MCComputeParams must be 80 bytes to match MSL layout");

    /// Result of a GPU Marching Cubes extraction.
    /// The caller takes ownership of vertexBuffer (must release when done,
    /// or adopt into a VertexBuffer which will manage its lifetime).
    struct MCExtractResult
    {
        MTL::Buffer* vertexBuffer = nullptr;  ///< 56-byte VertexData per vertex. Caller owns.
        uint32_t     vertexCount  = 0;        ///< Number of vertices (triangles*3).
        bool         success      = false;
    };

    /// Result of a multi-isovalue batch extraction.
    struct MCBatchResult
    {
        struct Layer {
            MTL::Buffer* vertexBuffer = nullptr;  ///< Caller takes ownership.
            uint32_t     vertexCount  = 0;
        };
        std::vector<Layer> layers;
        bool success = false;
    };

    /**
     * GPU Marching Cubes isosurface extraction via Metal compute kernels.
     *
     * Owns compute pipelines, lookup table buffers, and atomic counter buffer.
     * Each extract() call allocates a fresh output vertex buffer that is
     * transferred to the caller (per-extraction ownership).
     *
     * Thread-unsafe -- call from the main rendering thread only.
     */
    class MetalMarchingCubesPass
    {
    public:
        explicit MetalMarchingCubesPass(MetalGraphicsDevice* device);
        ~MetalMarchingCubesPass();

        // Non-copyable
        MetalMarchingCubesPass(const MetalMarchingCubesPass&) = delete;
        MetalMarchingCubesPass& operator=(const MetalMarchingCubesPass&) = delete;

        /// Extract an isosurface from a 3D volume texture.
        /// Each call allocates a new output buffer — the caller takes ownership.
        ///
        /// @param volumeTexture  3D texture (R32Float) containing scalar volume data.
        /// @param params         Extraction parameters (isovalue, dims, domain, etc.).
        /// @return               Extraction result with vertex buffer and count.
        MCExtractResult extract(Texture* volumeTexture, const MCComputeParams& params);

        /// Extract multiple isosurfaces in a single batch call.
        /// Each layer gets its own output buffer — the caller takes ownership.
        ///
        /// @param volumeTexture  3D texture (R32Float) containing scalar volume data.
        /// @param baseParams     Base extraction parameters (dims, domain, texelSize shared).
        /// @param isovalues      Isovalue for each layer.
        /// @param flipNormals    Per-layer flip normal flags (same size as isovalues).
        /// @return               Batch result with per-layer vertex buffers and counts.
        MCBatchResult extractBatch(Texture* volumeTexture,
                                   const MCComputeParams& baseParams,
                                   const std::vector<float>& isovalues,
                                   const std::vector<bool>& flipNormals = {});

        /// True once ensureResources() has succeeded.
        [[nodiscard]] bool isReady() const { return resourcesReady_; }

    private:
        void ensureResources();

        /// Allocate a new vertex buffer for a single extraction.
        /// Returns nullptr on failure. Caller takes ownership.
        MTL::Buffer* allocateVertexBuffer(uint32_t vertexCount);

        MetalGraphicsDevice* device_;

        // Compute pipelines
        MTL::ComputePipelineState* classifyPipeline_ = nullptr;
        MTL::ComputePipelineState* generatePipeline_ = nullptr;

        // Lookup table buffers (constant, created once)
        MTL::Buffer* edgeTableBuffer_ = nullptr;   // 256 x uint16_t = 512 bytes
        MTL::Buffer* triTableBuffer_  = nullptr;    // 256 x 16 x int8_t = 4096 bytes

        // Shared buffers (reused across extractions)
        MTL::Buffer* counterBuffer_   = nullptr;    // single atomic_uint (StorageModeShared)
        MTL::Buffer* uniformBuffer_   = nullptr;    // MCComputeParams (80 bytes)

        // Sampler for volume texture
        MTL::SamplerState* fieldSampler_ = nullptr;

        bool resourcesReady_ = false;
    };

} // namespace visutwin::canvas
