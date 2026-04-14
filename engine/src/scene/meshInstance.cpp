// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 21.12.2025.
//
#include "meshInstance.h"

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/instanceCuller.h"
#include "platform/graphics/vertexFormat.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    MeshInstance::MeshInstance(Mesh* mesh, Material* material, GraphNode* node)
        : _material(material), _mesh(mesh), _node(node)
    {
    }

    void MeshInstance::enableGpuInstanceCulling(GraphicsDevice* device, float boundingSphereRadius)
    {
        if (!device) {
            spdlog::warn("[MeshInstance] enableGpuInstanceCulling called with null device");
            return;
        }
        if (!_instancingData.vertexBuffer || _instancingData.count <= 0) {
            spdlog::warn("[MeshInstance] enableGpuInstanceCulling: setInstancing() must be called first");
            return;
        }
        if (!device->supportsGpuInstanceCulling()) {
            spdlog::info("[MeshInstance] Backend does not support GPU instance culling — skipping");
            return;
        }

        _instanceCuller = device->createInstanceCuller();
        if (!_instanceCuller) {
            spdlog::warn("[MeshInstance] Failed to create InstanceCuller");
            return;
        }

        // Pre-allocate the compacted buffer so we can wrap it as a VertexBuffer
        // before the first frame. The underlying native buffer stays stable
        // unless the instance count grows beyond this capacity.
        const uint32_t capacity = static_cast<uint32_t>(_instancingData.count);
        _instanceCuller->reserve(capacity);

        void* compactedNative = _instanceCuller->compactedNativeBuffer();
        void* indirectNative  = _instanceCuller->indirectArgsNativeBuffer();
        if (!compactedNative || !indirectNative) {
            spdlog::warn("[MeshInstance] Culler did not allocate buffers after reserve()");
            _instanceCuller.reset();
            return;
        }

        // Wrap the compacted MTL::Buffer* as a VertexBuffer. Reuse the same
        // instancing-formatted layout the source buffer has so the vertex
        // descriptor treats it identically (slot 5, per-instance step).
        auto format = _instancingData.vertexBuffer->format();
        _cachedCompactedVb = device->createVertexBufferFromNativeBuffer(
            format, _instancingData.count, compactedNative);
        if (!_cachedCompactedVb) {
            spdlog::warn("[MeshInstance] Failed to wrap compacted buffer as VertexBuffer");
            _instanceCuller.reset();
            return;
        }

        // Wire the indirect draw path: renderer.cpp:818 takes the indirect
        // branch iff all three of these are set.
        setIndirectInstancing(_cachedCompactedVb, indirectNative, /*slot=*/0);

        _instanceCullRadius = boundingSphereRadius;
        _gpuCullingEnabled = true;

        spdlog::info("[MeshInstance] GPU instance culling enabled: {} instances, radius={:.2f}",
            _instancingData.count, boundingSphereRadius);
    }

    BoundingBox MeshInstance::aabb()
    {
        // Use specified world space aabb
        if (!_updateAabb) {
            return _aabb;
        }

        // Callback function returning world space aabb
        if (_updateAabbFunc) {
            return _updateAabbFunc(_aabb);
        }

        // Use local space override aabb if specified
        BoundingBox localAabbStorage;
        BoundingBox* localAabb = _customAabb;
        bool toWorldSpace = (localAabb != nullptr);

        // Otherwise evaluate local aabb
        if (!localAabb) {
            localAabb = &localAabbStorage;

            if (_skinInstance) {
                if (_mesh) {
                    localAabb->setCenter(_mesh->aabb().center());
                    localAabb->setHalfExtents(_mesh->aabb().halfExtents());
                } else {
                    localAabb->setCenter(0, 0, 0);
                    localAabb->setHalfExtents(0, 0, 0);
                }
                // Note: skinned AABB calculation not yet implemented.
                // Requires accessing bone AABBs and transforming them.
                toWorldSpace = true;
            } else if (_node && (_aabbVer != _node->aabbVer() || (_mesh && _aabbMeshVer != _mesh->aabbVer()))) {
                // Local space bounding box from mesh
                if (_mesh) {
                    localAabb->setCenter(_mesh->aabb().center());
                    localAabb->setHalfExtents(_mesh->aabb().halfExtents());
                } else {
                    localAabb->setCenter(0, 0, 0);
                    localAabb->setHalfExtents(0, 0, 0);
                }

                // Note: morph target AABB expansion not yet implemented.
                // if (_mesh && _mesh->morph) {
                //     const BoundingBox& morphAabb = _mesh->morph->aabb;
                //     localAabb->_expand(morphAabb.getMin(), morphAabb.getMax());
                // }

                toWorldSpace = true;
                if (_node) {
                    _aabbVer = _node->aabbVer();
                }
                if (_mesh) {
                    _aabbMeshVer = _mesh->aabbVer();
                }
            }
        }

        // Store world space bounding box
        if (toWorldSpace && _node) {
            _aabb.setFromTransformedAabb(*localAabb, _node->worldTransform());
        }

        return _aabb;
    }
}
