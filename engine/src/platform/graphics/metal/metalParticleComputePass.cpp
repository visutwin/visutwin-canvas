// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Metal compute pass for GPU particle advection — implementation.
//
// Contains the embedded MSL compute kernel and the CPU-side dispatch logic.
//
// Custom shader — no upstream GLSL equivalent exists.
//
#include "metalParticleComputePass.h"

#include "metalGraphicsDevice.h"
#include "metalTexture.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    namespace
    {
        // ── Embedded Metal Shading Language ─────────────────────────────
        //
        // Compute kernel: advect particles through a 3D velocity field.
        //
        // Each thread processes one particle.  Integration uses classical
        // RK4 with hardware trilinear interpolation of the velocity
        // texture (RGBA32Float: xyz = velocity, w = magnitude).
        //
        // Particle lifecycle:
        //   - age += dt  each step
        //   - If age >= lifetime → respawn at seedPosition with new lifetime
        //   - If position exits domain → respawn
        //
        // The output buffer is suitable for direct rendering as point
        // primitives (position is at offset 0).
        //
        constexpr const char* PARTICLE_COMPUTE_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

// Must match GPUParticle in particleSystem3d.h (48 bytes, 16-byte aligned).
struct Particle {
    packed_float3 position;     // 12
    float         age;          //  4  → 16
    packed_float3 velocity;     //  12
    float         lifetime;     //  4  → 32
    packed_float3 seedPosition; //  12
    uint          flags;        //  4  → 48
};

struct Uniforms {
    packed_float3 domainMin;
    float         dt;
    packed_float3 domainMax;
    uint          particleCount;
    packed_float3 invDomainSize;
    float         time;
    float         speedMin;
    float         speedMax;
    float         fadeStart;
    float         padding;
};

// Convert world position to [0,1] texture coordinates for the velocity field.
inline float3 worldToUVW(float3 pos, float3 dMin, float3 invSize)
{
    return (pos - dMin) * invSize;
}

// Sample velocity field at a world-space position.
// Returns zero if outside domain.
inline float3 sampleVelocity(float3 worldPos,
                             texture3d<float> field,
                             sampler fieldSampler,
                             float3 dMin, float3 dMax, float3 invSize)
{
    float3 uvw = worldToUVW(worldPos, dMin, invSize);
    // Clamp to [0,1] — out-of-bounds positions get boundary velocity
    uvw = clamp(uvw, float3(0.0), float3(1.0));
    return field.sample(fieldSampler, uvw).xyz;
}

// Simple hash for per-particle pseudo-random lifetime variation.
inline float hashFloat(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return float(seed) / float(0xFFFFFFFFu);
}

kernel void advectParticles(
    device Particle*          particles      [[buffer(0)]],
    constant Uniforms&        uniforms       [[buffer(1)]],
    texture3d<float>          velocityField  [[texture(0)]],
    sampler                   fieldSampler   [[sampler(0)]],
    uint                      gid            [[thread_position_in_grid]])
{
    if (gid >= uniforms.particleCount) return;

    Particle p = particles[gid];

    // Skip dead particles
    if ((p.flags & 1u) == 0u) return;

    const float dt = uniforms.dt;
    const float3 dMin = float3(uniforms.domainMin);
    const float3 dMax = float3(uniforms.domainMax);
    const float3 invSize = float3(uniforms.invDomainSize);

    // ── Age ──────────────────────────────────────────────────────────
    p.age += dt;

    // ── Respawn if expired ───────────────────────────────────────────
    if (p.age >= p.lifetime) {
        p.position = p.seedPosition;
        p.velocity = packed_float3(0.0);
        p.age = 0.0;
        // Vary lifetime using particle index + time as seed
        float h = hashFloat(gid + as_type<uint>(uniforms.time));
        float minLife = 2.0;   // seconds
        float maxLife = 8.0;
        p.lifetime = minLife + h * (maxLife - minLife);
        particles[gid] = p;
        return;
    }

    // ── RK4 advection ────────────────────────────────────────────────
    float3 pos = float3(p.position);

    float3 k1 = sampleVelocity(pos,                         velocityField, fieldSampler, dMin, dMax, invSize);
    float3 k2 = sampleVelocity(pos + 0.5 * dt * k1,        velocityField, fieldSampler, dMin, dMax, invSize);
    float3 k3 = sampleVelocity(pos + 0.5 * dt * k2,        velocityField, fieldSampler, dMin, dMax, invSize);
    float3 k4 = sampleVelocity(pos + dt * k3,               velocityField, fieldSampler, dMin, dMax, invSize);

    float3 newPos = pos + (dt / 6.0) * (k1 + 2.0*k2 + 2.0*k3 + k4);

    // ── Boundary check ───────────────────────────────────────────────
    float3 uvw = worldToUVW(newPos, dMin, invSize);
    if (any(uvw < float3(0.0)) || any(uvw > float3(1.0))) {
        // Respawn at seed
        p.position = p.seedPosition;
        p.velocity = packed_float3(0.0);
        p.age = 0.0;
        float h = hashFloat(gid + as_type<uint>(uniforms.time) + 0x12345678u);
        p.lifetime = 2.0 + h * 6.0;
    } else {
        p.position = packed_float3(newPos);
        p.velocity = packed_float3(sampleVelocity(newPos, velocityField, fieldSampler, dMin, dMax, invSize));
    }

    particles[gid] = p;
}
)";

        constexpr uint32_t THREADS_PER_GROUP = 256;

    } // anonymous namespace

    // ─── Construction / Destruction ───────────────────────────────────

    MetalParticleComputePass::MetalParticleComputePass(MetalGraphicsDevice* device)
        : device_(device)
    {
    }

    MetalParticleComputePass::~MetalParticleComputePass()
    {
        if (particleBufferA_) { particleBufferA_->release(); particleBufferA_ = nullptr; }
        if (particleBufferB_) { particleBufferB_->release(); particleBufferB_ = nullptr; }
        if (uniformBuffer_)   { uniformBuffer_->release();   uniformBuffer_ = nullptr; }
        if (computePipeline_) { computePipeline_->release(); computePipeline_ = nullptr; }
        if (fieldSampler_)    { fieldSampler_->release();    fieldSampler_ = nullptr; }
    }

    // ─── Lazy Resource Creation ──────────────────────────────────────

    void MetalParticleComputePass::ensureResources()
    {
        if (resourcesReady_) return;

        auto* mtlDevice = device_->raw();
        if (!mtlDevice) return;

        // ── Compile compute shader ──────────────────────────────────
        if (!computePipeline_) {
            NS::Error* error = nullptr;
            auto* source = NS::String::string(
                PARTICLE_COMPUTE_SOURCE, NS::UTF8StringEncoding);
            auto* library = mtlDevice->newLibrary(source, nullptr, &error);
            if (!library) {
                spdlog::error("[MetalParticleComputePass] Failed to compile compute shader: {}",
                    error ? error->localizedDescription()->utf8String() : "unknown");
                return;
            }

            auto* funcName = NS::String::string("advectParticles", NS::UTF8StringEncoding);
            auto* function = library->newFunction(funcName);
            if (!function) {
                spdlog::error("[MetalParticleComputePass] Entry point 'advectParticles' not found");
                library->release();
                return;
            }

            computePipeline_ = mtlDevice->newComputePipelineState(function, &error);
            if (!computePipeline_) {
                spdlog::error("[MetalParticleComputePass] Failed to create pipeline state: {}",
                    error ? error->localizedDescription()->utf8String() : "unknown");
            }

            function->release();
            library->release();
        }

        // ── Uniform buffer (64 bytes) ───────────────────────────────
        if (!uniformBuffer_) {
            uniformBuffer_ = mtlDevice->newBuffer(
                sizeof(ParticleComputeUniforms),
                MTL::ResourceStorageModeShared);
        }

        // ── Trilinear sampler for velocity field ────────────────────
        if (!fieldSampler_) {
            auto* desc = MTL::SamplerDescriptor::alloc()->init();
            desc->setMinFilter(MTL::SamplerMinMagFilterLinear);
            desc->setMagFilter(MTL::SamplerMinMagFilterLinear);
            desc->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
            desc->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
            desc->setRAddressMode(MTL::SamplerAddressModeClampToEdge);
            fieldSampler_ = mtlDevice->newSamplerState(desc);
            desc->release();
        }

        resourcesReady_ = (computePipeline_ && uniformBuffer_ && fieldSampler_);
    }

    // ─── Buffer Initialization ───────────────────────────────────────

    void MetalParticleComputePass::initialize(uint32_t maxParticles)
    {
        auto* mtlDevice = device_->raw();
        if (!mtlDevice || maxParticles == 0) return;

        // Release old buffers
        if (particleBufferA_) { particleBufferA_->release(); particleBufferA_ = nullptr; }
        if (particleBufferB_) { particleBufferB_->release(); particleBufferB_ = nullptr; }

        const size_t bufferSize = static_cast<size_t>(maxParticles) * 48; // sizeof(GPUParticle)

        // Shared storage for CPU upload + GPU compute read/write.
        // Apple Silicon unified memory makes this zero-copy.
        particleBufferA_ = mtlDevice->newBuffer(bufferSize, MTL::ResourceStorageModeShared);
        particleBufferB_ = mtlDevice->newBuffer(bufferSize, MTL::ResourceStorageModeShared);

        if (!particleBufferA_ || !particleBufferB_) {
            spdlog::error("[MetalParticleComputePass] Failed to allocate particle buffers "
                          "({} particles, {} bytes each)", maxParticles, bufferSize);
            return;
        }

        // Zero-fill
        std::memset(particleBufferA_->contents(), 0, bufferSize);
        std::memset(particleBufferB_->contents(), 0, bufferSize);

        maxParticles_ = maxParticles;
        currentBuffer_ = 0;
        initialized_ = true;

        spdlog::info("[MetalParticleComputePass] Initialized: {} particles, {:.1f} MB per buffer",
            maxParticles, static_cast<double>(bufferSize) / (1024.0 * 1024.0));
    }

    // ─── CPU Upload ──────────────────────────────────────────────────

    void MetalParticleComputePass::uploadParticles(const void* data, uint32_t count)
    {
        if (!initialized_ || !data || count == 0) return;

        const size_t copySize = static_cast<size_t>(std::min(count, maxParticles_)) * 48;
        auto* dst = currentBuffer_ == 0 ? particleBufferA_ : particleBufferB_;
        std::memcpy(dst->contents(), data, copySize);
    }

    // ─── GPU Advection ───────────────────────────────────────────────

    void MetalParticleComputePass::advect(Texture* velocityTexture,
                                           const ParticleComputeUniforms& uniforms)
    {
        if (!initialized_ || !velocityTexture) return;

        ensureResources();
        if (!resourcesReady_) return;

        // Upload uniforms
        std::memcpy(uniformBuffer_->contents(), &uniforms, sizeof(ParticleComputeUniforms));

        // Select buffers: read from current, write to current (in-place).
        // Metal compute has no read/write hazard within a single dispatch
        // because each thread writes only its own particle.
        auto* particleBuffer = currentBuffer_ == 0 ? particleBufferA_ : particleBufferB_;

        // ── Encode compute command ──────────────────────────────────
        auto* commandBuffer = device_->_commandQueue->commandBuffer();
        if (!commandBuffer) {
            spdlog::warn("[MetalParticleComputePass] Failed to allocate command buffer");
            return;
        }

        auto* encoder = commandBuffer->computeCommandEncoder();
        if (!encoder) {
            spdlog::warn("[MetalParticleComputePass] Failed to create compute encoder");
            return;
        }

        encoder->pushDebugGroup(
            NS::String::string("ParticleAdvection", NS::UTF8StringEncoding));

        encoder->setComputePipelineState(computePipeline_);

        // Buffer bindings
        encoder->setBuffer(particleBuffer, 0, 0);   // [[buffer(0)]]
        encoder->setBuffer(uniformBuffer_, 0, 1);    // [[buffer(1)]]

        // Velocity field 3D texture
        auto* hwTexture = dynamic_cast<gpu::MetalTexture*>(velocityTexture->impl());
        if (hwTexture && hwTexture->raw()) {
            encoder->setTexture(hwTexture->raw(), 0);    // [[texture(0)]]
        }
        encoder->setSamplerState(fieldSampler_, 0);      // [[sampler(0)]]

        // Dispatch: one thread per particle
        const uint32_t threadgroups =
            (uniforms.particleCount + THREADS_PER_GROUP - 1) / THREADS_PER_GROUP;
        encoder->dispatchThreadgroups(
            MTL::Size(threadgroups, 1, 1),
            MTL::Size(THREADS_PER_GROUP, 1, 1));

        encoder->popDebugGroup();
        encoder->endEncoding();
        commandBuffer->commit();

        // We don't swap buffers since we advect in-place.
        // If double-buffering is needed for overlapping frames,
        // toggle currentBuffer_ = 1 - currentBuffer_ here.
    }

    // ─── Buffer Access ───────────────────────────────────────────────

    MTL::Buffer* MetalParticleComputePass::currentParticleBuffer() const
    {
        return currentBuffer_ == 0 ? particleBufferA_ : particleBufferB_;
    }

} // namespace visutwin::canvas
