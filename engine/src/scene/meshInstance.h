// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 21.12.2025.
//
#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <core/shape/boundingBox.h>

#include "graphNode.h"
#include "mesh.h"
#include "morphInstance.h"
#include "skinInstance.h"
#include "gsplat/gsplatInstance.h"
#include "scene/constants.h"
#include "materials/material.h"
#include "platform/graphics/vertexBuffer.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class GSplatInstance;
    class ParticleEmitter;
    class InstanceCuller;
    class SkinBatchInstance;
}

namespace visutwin::canvas
{
    /**
     * @brief Renderable instance of a Mesh with its own material, transform node, and optional GPU instancing.
     * @ingroup group_scene_renderer
     *
     * A single Mesh can be referenced by many MeshInstance objects, each with a different
     * material and transform (GraphNode). Hardware instancing and GPU-driven indirect
     * rendering are supported for drawing many copies with a single draw call.
     */
    class MeshInstance
    {
    public:
        /**
         *InstancingData class.
         * Holds the per-instance buffer and instance count for hardware instancing.
         * The vertex buffer contains packed InstanceData structs (float4x4 modelMatrix + float4 diffuseColor)
         * bound at buffer slot 5 and indexed by [[instance_id]] in the vertex shader.
         */
        struct InstancingData
        {
            std::shared_ptr<VertexBuffer> vertexBuffer;
            int count = 0;

            // GPU-driven indirect draw (Phase 3).
            // When indirectArgsBuffer != nullptr and indirectSlot >= 0,
            // renderer uses drawIndexedPrimitives(indirect:) instead of direct draw.
            void* indirectArgsBuffer = nullptr;  // MTL::Buffer* (opaque to avoid Metal headers in scene/)
            std::shared_ptr<VertexBuffer> compactedVertexBuffer;  // Culled output at slot 5
            int indirectSlot = -1;  // >= 0 activates indirect draw
        };

        MeshInstance(Mesh* mesh, Material* material, GraphNode* node = nullptr);

        // Shared-ownership variant: the instance co-owns mesh and material.
        // Use when the original owner (e.g. a GLB ContainerResource) can be
        // unloaded while entities instantiated from it still render.
        MeshInstance(std::shared_ptr<Mesh> mesh, std::shared_ptr<Material> material,
                     GraphNode* node = nullptr);

        BoundingBox aabb();

        Mesh* mesh() const { return _mesh; }

        Material* material() const { return _material; }

        /**
         * Replaces the material of this instance (upstream `meshInstance.material = ...`),
         * e.g. to render a loaded model with a custom ShaderMaterial. Drops any
         * shared ownership taken from the source container — the caller owns the
         * new material and must keep it alive.
         */
        void setMaterial(Material* material)
        {
            _material = material;
            _materialOwned.reset();
        }

        GraphNode* node() const { return _node; }

        bool castShadow() const { return _castShadow; }
        void setCastShadow(const bool value) { _castShadow = value; }

        //receiveShadow getter/setter.
        // When false, the SHADERDEF_NOSHADOW flag is set on the shader defs.
        bool receiveShadow() const { return _receiveShadow; }
        void setReceiveShadow(const bool value) { _receiveShadow = value; }

        bool cull() const { return _cull; }
        void setCull(const bool value) { _cull = value; }

        bool visibleThisFrame() const { return _visibleThisFrame; }
        void setVisibleThisFrame(const bool value) { _visibleThisFrame = value; }

        uint32_t mask() const { return _mask; }
        void setMask(const uint32_t value) { _mask = value; }

        // / instancingData / instancingCount.
        // Sets up hardware instancing for this mesh instance. The vertexBuffer must contain
        // packed InstanceData structs (80 bytes each: float4x4 + float4).
        // Also derives a world-space AABB covering the whole instance cloud, so
        // frustum culling and the directional shadow cascade fit see every instance
        // instead of the base mesh sitting at this instance's node transform. See
        // updateInstancingAabb — a caller that has already supplied its own AABB
        // (setCustomAabb) keeps it, and a buffer with no CPU-side copy is skipped.
        // DEVIATION: upstream leaves this to the app (MeshInstance.setCustomAabb).
        void setInstancing(const std::shared_ptr<VertexBuffer>& vertexBuffer, int count)
        {
            _instancingData.vertexBuffer = vertexBuffer;
            _instancingData.count = count;
            updateInstancingAabb();
        }

        // GPU-driven indirect instancing (Phase 3).
        // Sets up the mesh instance for indirect draw with a GPU-culled compacted buffer.
        // The compactedVB replaces the original instance buffer at slot 5.
        // indirectArgs is an opaque MTL::Buffer* containing MTLDrawIndexedPrimitivesIndirectArguments.
        void setIndirectInstancing(
            const std::shared_ptr<VertexBuffer>& compactedVB,
            void* indirectArgs, int slot = 0)
        {
            _instancingData.compactedVertexBuffer = compactedVB;
            _instancingData.indirectArgsBuffer = indirectArgs;
            _instancingData.indirectSlot = slot;
        }

        const InstancingData& instancingData() const { return _instancingData; }
        int instancingCount() const { return _instancingData.count; }

        // --- GPU instance culling ---
        //
        // Enable per-frame GPU frustum culling for this hardware-instanced mesh.
        // Must be called *after* setInstancing(vb, count). Every frame, the
        // renderer tests each instance's bounding sphere against the camera
        // frustum via a backend compute pass and writes only the visible
        // instances into a compacted buffer; the draw call then uses indirect
        // instancing (see Renderer::dispatchGpuInstanceCulling).
        //
        // boundingSphereRadius is the per-instance bounding sphere radius in
        // local space — typically the mesh's own bounding sphere radius
        // multiplied by the largest instance scale, plus a safety margin.
        //
        // Re-call this method if the source instance count changes — the
        // compacted buffer wrapper is sized once at enable time.
        void enableGpuInstanceCulling(GraphicsDevice* device, float boundingSphereRadius);

        bool gpuCullingEnabled() const { return _gpuCullingEnabled; }
        float instanceCullRadius() const { return _instanceCullRadius; }
        InstanceCuller* instanceCuller() const { return _instanceCuller.get(); }

        // --- GPU skinning ---

        /** Skin instance driving this mesh (shared across submeshes of one skinned node). */
        SkinInstance* skinInstance() const { return _skinInstance.get(); }

        /// Shared ownership of the skin, so a second MeshInstance can render the same
        /// mesh in the same animated pose (upstream shares meshInstance.skinInstance
        /// directly — e.g. an x-ray duplicate of a character in another layer).
        const std::shared_ptr<SkinInstance>& skinInstanceShared() const { return _skinInstance; }

        /**
         * Attach a skin instance. DEVIATION: also disables frustum culling for this
         * instance — the bind-pose AABB is not valid under animation and the port has
         * no bone-based AABB path yet (upstream recomputes the AABB from bones).
         */
        void setSkinInstance(const std::shared_ptr<SkinInstance>& skinInstance);

        // --- Gaussian splats ---

        GSplatInstance* gsplatInstance() const { return _gsplatInstance.get(); }
        void setGSplatInstance(const std::shared_ptr<GSplatInstance>& gsplatInstance)
        {
            _gsplatInstance = gsplatInstance;
        }

        // --- GPU particles ---

        ParticleEmitter* particleEmitter() const { return _particleEmitter.get(); }
        void setParticleEmitter(const std::shared_ptr<ParticleEmitter>& emitter)
        {
            _particleEmitter = emitter;
        }

        // --- App-driven storage draws ---

        /**
         * Draw `instanceCount` instances of this mesh with an app-owned storage buffer
         * bound for the vertex stage, plus a small parameter block. The custom shader
         * expands one instance per record in the buffer, keyed off the instance id —
         * the same mechanism the built-in particle emitter and Gaussian splats use, made
         * available to applications that simulate their own data with a compute shader.
         *
         * The buffer and parameter block share the emitter/splat binding slots, so a
         * mesh instance that has a storage draw must not also carry a particle emitter
         * or a splat instance. `params` is copied.
         */
        void setStorageDraw(const std::shared_ptr<VertexBuffer>& buffer, const int instanceCount,
            const void* params, const size_t paramsSize)
        {
            _storageBuffer = buffer;
            _storageDrawCount = buffer ? instanceCount : 0;
            _storageParams.assign(static_cast<const uint8_t*>(params),
                static_cast<const uint8_t*>(params) + paramsSize);
        }

        const std::shared_ptr<VertexBuffer>& storageBuffer() const { return _storageBuffer; }
        int storageDrawCount() const { return _storageDrawCount; }
        const std::vector<uint8_t>& storageParams() const { return _storageParams; }

        // --- Morph targets ---

        MorphInstance* morphInstance() const { return _morphInstance.get(); }

        /// Shared ownership of the morph state — see skinInstanceShared.
        const std::shared_ptr<MorphInstance>& morphInstanceShared() const { return _morphInstance; }
        void setMorphInstance(const std::shared_ptr<MorphInstance>& morphInstance)
        {
            _morphInstance = morphInstance;
        }

        // --- Batching support (batchGroupId, visible) ---

        int batchGroupId() const { return _batchGroupId; }
        void setBatchGroupId(int id) { _batchGroupId = id; }

        /** When false, the mesh instance is hidden (merged into a batch). */
        bool visible() const { return _visible; }
        void setVisible(bool v) { _visible = v; }

        // --- Dynamic batching support ---

        /** SkinBatchInstance pointer for dynamic batches (non-owning, owned by Batch). */
        SkinBatchInstance* skinBatchInstance() const { return _skinBatchInstance; }
        void setSkinBatchInstance(SkinBatchInstance* sbi) { _skinBatchInstance = sbi; }

        /** True when this mesh instance is part of a dynamic batch (triggers VT_FEATURE_DYNAMIC_BATCH). */
        bool isDynamicBatch() const { return _dynamicBatch; }
        void setDynamicBatch(bool v) { _dynamicBatch = v; }

        /** Override the computed AABB with a custom value (used by dynamic batch AABB updates). */
        void setCustomAabb(const BoundingBox& aabb) {
            _aabb = aabb;
            _updateAabb = false;
        }

    private:
        // Unions the mesh AABB transformed by every per-instance matrix into a
        // world-space _aabb override. Called by setInstancing.
        void updateInstancingAabb();

        Material* _material = nullptr;
        Mesh* _mesh = nullptr;

        // Optional co-ownership backing the raw pointers above (set by the
        // shared-ownership constructor). Keeps container-created resources
        // alive after the container itself is unloaded.
        std::shared_ptr<Mesh> _meshOwned;
        std::shared_ptr<Material> _materialOwned;

        // The graph node defining the transform for this instance.
        GraphNode* _node = nullptr;

        BoundingBox _aabb;
        bool _updateAabb = true;
        std::function<BoundingBox&(BoundingBox&)> _updateAabbFunc = nullptr;
        BoundingBox* _customAabb = nullptr;
        int _aabbVer = -1;
        int _aabbMeshVer = -1;

        std::shared_ptr<SkinInstance> _skinInstance;
        std::shared_ptr<MorphInstance> _morphInstance;
        std::shared_ptr<GSplatInstance> _gsplatInstance;
        std::shared_ptr<ParticleEmitter> _particleEmitter;
        std::shared_ptr<VertexBuffer> _storageBuffer;
        std::vector<uint8_t> _storageParams;
        int _storageDrawCount = 0;
        SkinBatchInstance* _skinBatchInstance = nullptr;
        InstancingData _instancingData;

        // GPU instance culling: per-instance culler owning compacted output +
        // indirect args buffers. _cachedCompactedVb wraps the culler's
        // compacted native buffer as a VertexBuffer so the existing indirect
        // draw path at renderer.cpp:818 can bind it at slot 5.
        std::unique_ptr<InstanceCuller> _instanceCuller;
        std::shared_ptr<VertexBuffer> _cachedCompactedVb;
        float _instanceCullRadius = 0.0f;
        bool _gpuCullingEnabled = false;

        bool _castShadow = true;
        bool _receiveShadow = true;
        bool _cull = true;
        bool _visibleThisFrame = false;
        uint32_t _mask = MASK_AFFECT_DYNAMIC;

        // Batching
        int _batchGroupId = -1;   // BatchGroup::NOID
        bool _visible = true;     // Hidden when merged into a batch
        bool _dynamicBatch = false;  // True when part of a dynamic batch
    };
}
