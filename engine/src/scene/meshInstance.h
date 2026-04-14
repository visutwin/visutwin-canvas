// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 21.12.2025.
//
#pragma once

#include <cstdint>
#include <memory>
#include <core/shape/boundingBox.h>

#include "graphNode.h"
#include "mesh.h"
#include "skinInstance.h"
#include "scene/constants.h"
#include "materials/material.h"
#include "platform/graphics/vertexBuffer.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
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

        BoundingBox aabb();

        Mesh* mesh() const { return _mesh; }

        Material* material() const { return _material; }

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
        void setInstancing(const std::shared_ptr<VertexBuffer>& vertexBuffer, int count)
        {
            _instancingData.vertexBuffer = vertexBuffer;
            _instancingData.count = count;
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
        // frustum via a Metal compute pass and writes only the visible
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
        Material* _material = nullptr;
        Mesh* _mesh = nullptr;

        // The graph node defining the transform for this instance.
        GraphNode* _node = nullptr;

        BoundingBox _aabb;
        bool _updateAabb = true;
        std::function<BoundingBox&(BoundingBox&)> _updateAabbFunc = nullptr;
        BoundingBox* _customAabb = nullptr;
        int _aabbVer = -1;
        int _aabbMeshVer = -1;

        SkinInstance* _skinInstance = nullptr;
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
