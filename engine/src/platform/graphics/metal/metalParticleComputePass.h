// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Metal compute pass for GPU particle advection.
//
// Advects millions of particles through a 3D velocity field entirely
// on the GPU using a Metal compute kernel.  The output particle buffer
// can be rendered directly as point primitives.
//
// Pipeline (per frame):
//   1. Encode compute command: bind particle buffer, velocity 3D texture,
//      uniforms, dispatch one thread per particle.
//   2. Commit command buffer.  Metal's implicit barriers ensure the
//      vertex read in the subsequent render pass sees the compute output.
//
// Double-buffered particle storage avoids read/write hazards when
// the previous frame's render pass hasn't finished yet.
//
// Follows the MetalLICPass / MetalTaaPass decomposition pattern:
//   - Embedded MSL source as string literal
//   - Lazy resource creation
//   - Friend access to MetalGraphicsDevice for command queue
//
// Custom shader — no upstream GLSL equivalent exists for GPU
// particle advection.
//
#pragma once

#include <cstdint>
#include <memory>
#include <Metal/Metal.hpp>

namespace visutwin::canvas
{
    class MetalGraphicsDevice;
    class Texture;

    /// Uniform data uploaded to the compute kernel each frame.
    struct alignas(16) ParticleComputeUniforms
    {
        float domainMin[3];     ///< Field domain minimum (world space).
        float dt;               ///< Integration timestep.
        float domainMax[3];     ///< Field domain maximum (world space).
        uint32_t particleCount; ///< Number of active particles.
        float invDomainSize[3]; ///< 1 / (domainMax - domainMin).
        float time;             ///< Current simulation time (for seeding noise).
        float speedMin;         ///< Minimum speed for TF mapping.
        float speedMax;         ///< Maximum speed for TF mapping.
        float fadeStart;        ///< Age ratio where alpha fade begins.
        float padding;          ///< Align to 64 bytes.
    };
    static_assert(sizeof(ParticleComputeUniforms) == 64,
        "ParticleComputeUniforms must be 64 bytes");

    /**
     * Manages GPU particle advection via a Metal compute kernel.
     *
     * Owns the compute pipeline, double-buffered particle MTL::Buffers,
     * and uniform buffer.  Advection, aging, respawning, and boundary
     * handling all happen on the GPU.
     */
    class MetalParticleComputePass
    {
    public:
        explicit MetalParticleComputePass(MetalGraphicsDevice* device);
        ~MetalParticleComputePass();

        // Non-copyable
        MetalParticleComputePass(const MetalParticleComputePass&) = delete;
        MetalParticleComputePass& operator=(const MetalParticleComputePass&) = delete;

        /// Allocate particle buffers for the given capacity.
        /// Must be called before the first advect().
        void initialize(uint32_t maxParticles);

        /// Upload initial particle data from CPU.
        /// @param data  Array of GPUParticle structs (48 bytes each).
        /// @param count Number of particles to upload.
        void uploadParticles(const void* data, uint32_t count);

        /// Execute GPU advection for one timestep.
        ///
        /// @param velocityTexture  3D texture (RGBA32Float): xyz = velocity, w unused.
        /// @param uniforms         Per-frame parameters.
        void advect(Texture* velocityTexture, const ParticleComputeUniforms& uniforms);

        /// Get the current (most recently written) particle buffer for rendering.
        /// The buffer contains `maxParticles` GPUParticle structs (48 bytes each).
        [[nodiscard]] MTL::Buffer* currentParticleBuffer() const;

        /// Get the particle count (set during initialize).
        [[nodiscard]] uint32_t maxParticles() const { return maxParticles_; }

        /// True once initialize() has been called successfully.
        [[nodiscard]] bool isInitialized() const { return initialized_; }

    private:
        void ensureResources();

        MetalGraphicsDevice* device_;

        MTL::ComputePipelineState* computePipeline_ = nullptr;
        MTL::Buffer* particleBufferA_ = nullptr;   // ping
        MTL::Buffer* particleBufferB_ = nullptr;   // pong
        MTL::Buffer* uniformBuffer_ = nullptr;
        MTL::SamplerState* fieldSampler_ = nullptr;

        uint32_t maxParticles_ = 0;
        uint32_t currentBuffer_ = 0;               // 0 = A is current, 1 = B
        bool initialized_ = false;
        bool resourcesReady_ = false;
    };

} // namespace visutwin::canvas
