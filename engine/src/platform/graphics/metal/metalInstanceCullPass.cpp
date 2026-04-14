// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Metal compute pass for GPU frustum culling -- implementation.
//
// Contains the embedded MSL compute kernels (instanceCull + writeIndirectArgs)
// and CPU-side dispatch logic for the two-kernel pipeline.
//
// Custom shader -- no upstream GLSL equivalent exists.
//
#include "metalInstanceCullPass.h"

#include "metalGraphicsDevice.h"
#include "metalVertexBuffer.h"
#include "spdlog/spdlog.h"

#include <cstring>

namespace visutwin::canvas
{
    namespace
    {
        // ── Embedded Metal Shading Language ─────────────────────────────
        //
        // Two compute kernels for GPU frustum culling of instances.
        //
        // Kernel 1 (instanceCull): Tests each instance bounding sphere vs 6 planes,
        //   compacts visible instances into an output buffer via atomic_fetch_add.
        // Kernel 2 (writeIndirectArgs): Reads the atomic counter and writes
        //   MTLDrawIndexedPrimitivesIndirectArguments.
        //
        // Instance layout matches InstanceData in common.metal (80 bytes):
        //   float4x4 modelMatrix (64 bytes) + float4 diffuseColor (16 bytes).
        //
        constexpr const char* INSTANCE_CULL_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

// ── Uniform parameters ──────────────────────────────────────────────
// Must match InstanceCullParams in metalInstanceCullPass.h (128 bytes).
struct CullParams {
    float4 frustumPlanes[6];    // (nx, ny, nz, d) per plane. dot(n,p)+d >= 0 = inside.
    float  boundingSphereRadius;
    uint   instanceCount;
    uint   indexCount;          // mesh Primitive.count -> indirect args
    uint   indexStart;          // mesh Primitive.base -> indirect args
    int    baseVertex;          // mesh Primitive.baseVertex -> indirect args
    uint   baseInstance;        // always 0
    float  _pad[2];
};

// ── Instance data layout (80 bytes) ────────────────────────────────
// Matches common.metal InstanceData.
struct InstanceData {
    float4x4 modelMatrix;      // 64 bytes
    float4   diffuseColor;     // 16 bytes
};

// ── Metal indirect draw arguments (20 bytes) ───────────────────────
// Matches MTLDrawIndexedPrimitivesIndirectArguments.
struct IndirectArgs {
    uint indexCount;
    uint instanceCount;
    uint indexStart;
    int  baseVertex;
    uint baseInstance;
};

// ── Kernel 1: Frustum cull instances and compact visible ones ──────
// 64 threads per threadgroup, 1D dispatch.
kernel void instanceCull(
    constant CullParams&     params     [[buffer(0)]],
    constant InstanceData*   input      [[buffer(1)]],
    device InstanceData*     output     [[buffer(2)]],
    device atomic_uint*      counter    [[buffer(3)]],
    uint                     tid        [[thread_position_in_grid]])
{
    if (tid >= params.instanceCount) return;

    // Extract instance world position from translation column of model matrix.
    const float3 center = float3(input[tid].modelMatrix[3][0],
                                  input[tid].modelMatrix[3][1],
                                  input[tid].modelMatrix[3][2]);
    const float radius = params.boundingSphereRadius;

    // Test bounding sphere against 6 frustum planes.
    // dot(plane.xyz, center) + plane.w + radius >= 0 means (partially) inside.
    for (int p = 0; p < 6; ++p) {
        const float4 plane = params.frustumPlanes[p];
        const float dist = dot(plane.xyz, center) + plane.w;
        if (dist < -radius) {
            return; // Outside this plane — culled.
        }
    }

    // Visible: atomically allocate a slot and copy instance data.
    const uint slot = atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
    output[slot] = input[tid];
}

// ── Kernel 2: Write indirect draw arguments ────────────────────────
// Single thread reads the atomic counter and writes the indirect args.
kernel void writeIndirectArgs(
    constant CullParams&     params     [[buffer(0)]],
    device atomic_uint*      counter    [[buffer(3)]],
    device IndirectArgs*     args       [[buffer(4)]],
    uint                     tid        [[thread_position_in_grid]])
{
    if (tid != 0) return;

    const uint visibleCount = atomic_load_explicit(counter, memory_order_relaxed);
    args[0].indexCount    = params.indexCount;
    args[0].instanceCount = visibleCount;
    args[0].indexStart    = params.indexStart;
    args[0].baseVertex    = params.baseVertex;
    args[0].baseInstance  = params.baseInstance;
}
)";

        constexpr uint32_t THREADGROUP_SIZE = 64;
        constexpr size_t INSTANCE_DATA_SIZE = 80; // bytes per instance

    } // anonymous namespace

    // ─── Construction / Destruction ───────────────────────────────────

    MetalInstanceCullPass::MetalInstanceCullPass(MetalGraphicsDevice* device)
        : device_(device)
    {
    }

    MetalInstanceCullPass::~MetalInstanceCullPass()
    {
        if (cullPipeline_)      { cullPipeline_->release();      cullPipeline_ = nullptr; }
        if (writeArgsPipeline_) { writeArgsPipeline_->release(); writeArgsPipeline_ = nullptr; }
        if (compactedBuffer_)   { compactedBuffer_->release();   compactedBuffer_ = nullptr; }
        if (indirectArgsBuffer_){ indirectArgsBuffer_->release(); indirectArgsBuffer_ = nullptr; }
        if (counterBuffer_)     { counterBuffer_->release();     counterBuffer_ = nullptr; }
        if (uniformBuffer_)     { uniformBuffer_->release();     uniformBuffer_ = nullptr; }
    }

    // ─── Lazy Resource Creation ──────────────────────────────────────

    void MetalInstanceCullPass::ensureResources()
    {
        if (resourcesReady_) return;

        auto* mtlDevice = device_->raw();
        if (!mtlDevice) return;

        // ── Compile compute shaders ────────────────────────────────
        if (!cullPipeline_ || !writeArgsPipeline_) {
            NS::Error* error = nullptr;
            auto* source = NS::String::string(INSTANCE_CULL_SOURCE, NS::UTF8StringEncoding);
            auto* library = mtlDevice->newLibrary(source, nullptr, &error);
            if (!library) {
                spdlog::error("[MetalInstanceCullPass] Failed to compile cull shaders: {}",
                    error ? error->localizedDescription()->utf8String() : "unknown");
                return;
            }

            // Cull pipeline
            if (!cullPipeline_) {
                auto* funcName = NS::String::string("instanceCull", NS::UTF8StringEncoding);
                auto* function = library->newFunction(funcName);
                if (!function) {
                    spdlog::error("[MetalInstanceCullPass] Entry point 'instanceCull' not found");
                    library->release();
                    return;
                }
                cullPipeline_ = mtlDevice->newComputePipelineState(function, &error);
                function->release();
                if (!cullPipeline_) {
                    spdlog::error("[MetalInstanceCullPass] Failed to create cull pipeline: {}",
                        error ? error->localizedDescription()->utf8String() : "unknown");
                    library->release();
                    return;
                }
            }

            // WriteArgs pipeline
            if (!writeArgsPipeline_) {
                auto* funcName = NS::String::string("writeIndirectArgs", NS::UTF8StringEncoding);
                auto* function = library->newFunction(funcName);
                if (!function) {
                    spdlog::error("[MetalInstanceCullPass] Entry point 'writeIndirectArgs' not found");
                    library->release();
                    return;
                }
                writeArgsPipeline_ = mtlDevice->newComputePipelineState(function, &error);
                function->release();
                if (!writeArgsPipeline_) {
                    spdlog::error("[MetalInstanceCullPass] Failed to create writeArgs pipeline: {}",
                        error ? error->localizedDescription()->utf8String() : "unknown");
                    library->release();
                    return;
                }
            }

            library->release();
        }

        // ── Atomic counter buffer (single uint32) ──────────────────
        if (!counterBuffer_) {
            counterBuffer_ = mtlDevice->newBuffer(sizeof(uint32_t), MTL::ResourceStorageModeShared);
            if (!counterBuffer_) {
                spdlog::error("[MetalInstanceCullPass] Failed to create counter buffer");
                return;
            }
        }

        // ── Uniform buffer (InstanceCullParams) ────────────────────
        if (!uniformBuffer_) {
            uniformBuffer_ = mtlDevice->newBuffer(sizeof(InstanceCullParams), MTL::ResourceStorageModeShared);
            if (!uniformBuffer_) {
                spdlog::error("[MetalInstanceCullPass] Failed to create uniform buffer");
                return;
            }
        }

        // ── Indirect args buffer (20 bytes) ────────────────────────
        if (!indirectArgsBuffer_) {
            indirectArgsBuffer_ = mtlDevice->newBuffer(
                5 * sizeof(uint32_t), MTL::ResourceStorageModeShared);
            if (!indirectArgsBuffer_) {
                spdlog::error("[MetalInstanceCullPass] Failed to create indirect args buffer");
                return;
            }
        }

        resourcesReady_ = (cullPipeline_ && writeArgsPipeline_ &&
                           counterBuffer_ && uniformBuffer_ && indirectArgsBuffer_);

        if (resourcesReady_) {
            spdlog::info("[MetalInstanceCullPass] Resources initialized successfully");
        }
    }

    // ─── Buffer Reservation ──────────────────────────────────────────

    void MetalInstanceCullPass::reserve(uint32_t maxInstances)
    {
        // Make sure pipelines + the fixed-size buffers (counter, uniform,
        // indirect args) exist — callers that invoke reserve() upfront
        // (MeshInstance::enableGpuInstanceCulling) rely on all native buffer
        // pointers being valid before any cull() dispatch.
        ensureResources();
        if (!resourcesReady_) return;

        if (maxInstances <= maxInstances_ && compactedBuffer_) return;

        auto* mtlDevice = device_->raw();
        if (!mtlDevice) return;

        if (compactedBuffer_) {
            compactedBuffer_->release();
            compactedBuffer_ = nullptr;
        }

        const size_t bufferSize = static_cast<size_t>(maxInstances) * INSTANCE_DATA_SIZE;
        compactedBuffer_ = mtlDevice->newBuffer(bufferSize, MTL::ResourceStorageModeShared);
        if (!compactedBuffer_) {
            spdlog::error("[MetalInstanceCullPass] Failed to allocate compacted buffer ({} instances, {:.1f} KB)",
                maxInstances, static_cast<double>(bufferSize) / 1024.0);
            maxInstances_ = 0;
            return;
        }

        maxInstances_ = maxInstances;
        spdlog::debug("[MetalInstanceCullPass] Reserved compacted buffer for {} instances ({:.1f} KB)",
            maxInstances, static_cast<double>(bufferSize) / 1024.0);
    }

    // ─── GPU Culling ─────────────────────────────────────────────────

    void MetalInstanceCullPass::cull(VertexBuffer* input, const InstanceCullParams& params)
    {
        if (!input) return;
        // All VertexBuffers on the Metal backend are MetalVertexBuffer.
        auto* mv = static_cast<MetalVertexBuffer*>(input);
        cullRaw(mv->raw(), params);
    }

    void MetalInstanceCullPass::cullRaw(MTL::Buffer* inputBuffer, const InstanceCullParams& params)
    {
        if (!inputBuffer || params.instanceCount == 0) return;

        ensureResources();
        if (!resourcesReady_) return;

        // Ensure compacted buffer is large enough
        reserve(params.instanceCount);
        if (!compactedBuffer_) return;

        // Upload uniforms
        std::memcpy(uniformBuffer_->contents(), &params, sizeof(InstanceCullParams));

        // Reset atomic counter to 0
        uint32_t zero = 0;
        std::memcpy(counterBuffer_->contents(), &zero, sizeof(uint32_t));

        // Dispatch kernel 1 + kernel 2 in a single command buffer.
        // Metal guarantees sequential execution of compute encoders within
        // the same command buffer — no explicit barrier needed.
        auto* commandBuffer = device_->_commandQueue->commandBuffer();
        if (!commandBuffer) {
            spdlog::warn("[MetalInstanceCullPass] Failed to create command buffer");
            return;
        }

        // ── Kernel 1: Frustum cull instances ────────────────────────
        {
            auto* encoder = commandBuffer->computeCommandEncoder();
            if (!encoder) {
                spdlog::warn("[MetalInstanceCullPass] Failed to create compute encoder for cull");
                return;
            }

            encoder->pushDebugGroup(
                NS::String::string("InstanceCull", NS::UTF8StringEncoding));

            encoder->setComputePipelineState(cullPipeline_);
            encoder->setBuffer(uniformBuffer_,     0, 0);  // [[buffer(0)]] params
            encoder->setBuffer(inputBuffer,        0, 1);  // [[buffer(1)]] input
            encoder->setBuffer(compactedBuffer_,   0, 2);  // [[buffer(2)]] output
            encoder->setBuffer(counterBuffer_,     0, 3);  // [[buffer(3)]] counter

            const uint32_t threadgroups = (params.instanceCount + THREADGROUP_SIZE - 1) / THREADGROUP_SIZE;
            encoder->dispatchThreadgroups(
                MTL::Size(threadgroups, 1, 1),
                MTL::Size(THREADGROUP_SIZE, 1, 1));

            encoder->popDebugGroup();
            encoder->endEncoding();
        }

        // ── Kernel 2: Write indirect draw arguments ─────────────────
        {
            auto* encoder = commandBuffer->computeCommandEncoder();
            if (!encoder) {
                spdlog::warn("[MetalInstanceCullPass] Failed to create compute encoder for writeArgs");
                return;
            }

            encoder->pushDebugGroup(
                NS::String::string("WriteIndirectArgs", NS::UTF8StringEncoding));

            encoder->setComputePipelineState(writeArgsPipeline_);
            encoder->setBuffer(uniformBuffer_,      0, 0);  // [[buffer(0)]] params
            encoder->setBuffer(counterBuffer_,      0, 3);  // [[buffer(3)]] counter
            encoder->setBuffer(indirectArgsBuffer_, 0, 4);  // [[buffer(4)]] args

            encoder->dispatchThreadgroups(
                MTL::Size(1, 1, 1),
                MTL::Size(1, 1, 1));

            encoder->popDebugGroup();
            encoder->endEncoding();
        }

        // MVP: synchronous wait. For production, this could be replaced with
        // a shared event or fence to overlap compute with the previous frame's render.
        commandBuffer->commit();
        commandBuffer->waitUntilCompleted();
    }

    // Note: extractFrustumPlanes lives in instanceCuller.cpp so it can be
    // reused by future non-Metal backends.

    uint32_t MetalInstanceCullPass::visibleCountReadback() const
    {
        if (!indirectArgsBuffer_) return 0;
        // Indirect args layout (MTLDrawIndexedPrimitivesIndirectArguments):
        //   uint indexCount;      // offset 0
        //   uint instanceCount;   // offset 4   <-- visible instance count
        //   uint indexStart;      // offset 8
        //   int  baseVertex;      // offset 12
        //   uint baseInstance;    // offset 16
        const auto* bytes = static_cast<const uint8_t*>(indirectArgsBuffer_->contents());
        if (!bytes) return 0;
        uint32_t instanceCount = 0;
        std::memcpy(&instanceCount, bytes + 4, sizeof(uint32_t));
        return instanceCount;
    }

} // namespace visutwin::canvas
