// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025.
//
//
//
#include "batchManager.h"
#include "batchSplit.h"
#include "skinBatchInstance.h"

#include <algorithm>
#include <unordered_map>

#include "framework/components/render/renderComponent.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/composition/layerComposition.h"
#include "scene/scene.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    std::vector<MeshInstance*> BatchManager::_batchMeshInstances;


    /**
     * Consistent vertex layout used by all parsers (GLB, Assimp, OBJ, STL).
     * 56 bytes = 14 floats: position[3] + normal[3] + uv0[2] + tangent[4] + uv1[2].
     */
    struct PackedVertex
    {
        float px, py, pz;       // position
        float nx, ny, nz;       // normal
        float u, v;             // uv0
        float tx, ty, tz, tw;   // tangent + handedness
        float u1, v1;           // uv1
    };

    static_assert(sizeof(PackedVertex) == 56, "PackedVertex must be 56 bytes (14 floats)");

    /**
     * Extended vertex layout for dynamic batching.
     * 60 bytes = PackedVertex (56) + boneIndex (4).
     * The boneIndex is a float encoding the mesh instance index into the matrix palette.
     * Used only during dynamic batch merging — does not affect existing parsers or static batching.
     *uses SEMANTIC_BLENDINDICES for the same purpose.
     */
    struct DynamicBatchVertex
    {
        float px, py, pz;       // position
        float nx, ny, nz;       // normal
        float u, v;             // uv0
        float tx, ty, tz, tw;   // tangent + handedness
        float u1, v1;           // uv1
        float boneIndex;        // mesh instance index into palette
    };

    static_assert(sizeof(DynamicBatchVertex) == 60, "DynamicBatchVertex must be 60 bytes (15 floats)");

    static_assert(sizeof(PackedVertex) == kPackedVertexStride,
        "batchSplit's packed stride must be the struct the merge paths read");

    namespace
    {
        /**
         * Both merge loops reinterpret a source vertex buffer as `PackedVertex[]`.
         * That is only meaningful when the buffer IS that layout, so ask rather than
         * assume: a skinned mesh (88 bytes), a point cloud (28) or any custom format
         * tagged into a batch group would otherwise be merged as garbage geometry,
         * reading past the end of its own storage on the way. The storage check is the
         * memory-safety half — the cast walks numVertices() strides, and nothing else
         * guarantees the bytes are there.
         */
        bool isPackedVertexLayout(const VertexBuffer* vertexBuffer)
        {
            if (!vertexBuffer || !vertexBuffer->format()) {
                return false;
            }
            if (!formatIsPackedVertexLayout(*vertexBuffer->format())) {
                return false;
            }
            const size_t needed = static_cast<size_t>(vertexBuffer->numVertices()) * sizeof(PackedVertex);
            return vertexBuffer->storage().size() >= needed;
        }

        /**
         * True when every mesh instance in the list can be merged. Reports the first
         * one that cannot, because a rejected batch is invisible otherwise: the
         * originals simply stay unbatched and the scene renders correctly but slower.
         */
        bool listIsMergeable(const std::vector<MeshInstance*>& meshInstances)
        {
            for (const auto* mi : meshInstances) {
                const auto casterName = mi->node() ? mi->node()->name() : std::string("<no node>");
                const auto vertexBuffer = mi->mesh()->getVertexBuffer();
                if (!isPackedVertexLayout(vertexBuffer.get())) {
                    spdlog::warn("[BatchManager] Skipping batch: '{}' has a {}-byte vertex layout "
                                 "that is not the {}-byte packed one merging reads "
                                 "(position, normal, uv0, tangent, uv1)",
                                 casterName,
                                 vertexBuffer && vertexBuffer->format() ? vertexBuffer->format()->size() : 0,
                                 static_cast<int>(sizeof(PackedVertex)));
                    return false;
                }
                // The merged mesh declares PRIMITIVE_TRIANGLES and rebuilds its index
                // buffer, so a strip or a fan would come out as unrelated triangles.
                if (mi->mesh()->getPrimitive().type != PrimitiveType::PRIMITIVE_TRIANGLES) {
                    spdlog::warn("[BatchManager] Skipping batch: '{}' is primitive type {}, "
                                 "and merging produces triangle lists",
                                 casterName,
                                 static_cast<int>(mi->mesh()->getPrimitive().type));
                    return false;
                }
            }
            return true;
        }

        /** Describes one mesh instance for splitBatchLists(). */
        BatchCandidate describeCandidate(MeshInstance* meshInstance)
        {
            BatchCandidate candidate;
            const auto vertexBuffer = meshInstance->mesh()->getVertexBuffer();
            candidate.formatBatchingHash = vertexBuffer && vertexBuffer->format()
                ? vertexBuffer->format()->batchingHash() : 0u;
            candidate.primitiveType = static_cast<int>(meshInstance->mesh()->getPrimitive().type);
            candidate.castShadow = meshInstance->castShadow();
            candidate.receiveShadow = meshInstance->receiveShadow();
            candidate.aabb = meshInstance->aabb();
            return candidate;
        }
    }

    // -------------------------------------------------------------------------

    BatchManager::BatchManager(GraphicsDevice* device) : _device(device) {}

    void BatchManager::addGroup(const BatchGroup& group)
    {
        _groups[group.id] = group;
        // Only meaningful once prepare() has run: markGroupDirty is picked up by
        // updateAll, which needs a scene, and during setup the app has not tagged
        // anything with this group yet.
        markGroupDirty(group.id);
    }

    void BatchManager::removeGroup(int groupId)
    {
        std::erase(_dirtyGroups, groupId);
        // Destroy any batches belonging to this group first.
        for (auto it = _batches.begin(); it != _batches.end(); ) {
            if ((*it)->batchGroupId == groupId) {
                // Restore originals.
                for (auto* mi : (*it)->origMeshInstances) {
                    mi->setVisible(true);
                }
                it = _batches.erase(it);
            } else {
                ++it;
            }
        }
        _groups.erase(groupId);
    }

    const BatchGroup* BatchManager::getGroupById(int groupId) const
    {
        auto it = _groups.find(groupId);
        return it != _groups.end() ? &it->second : nullptr;
    }

    // -------------------------------------------------------------------------
    // prepare() — 
    // -------------------------------------------------------------------------
    void BatchManager::prepare(Scene* scene)
    {
        std::vector<int> allGroups;
        allGroups.reserve(_groups.size());
        for (const auto& [groupId, group] : _groups) {
            (void)group;
            allGroups.push_back(groupId);
        }
        generate(scene, allGroups);
    }

    void BatchManager::markGroupDirty(const int groupId)
    {
        if (std::find(_dirtyGroups.begin(), _dirtyGroups.end(), groupId) == _dirtyGroups.end()) {
            _dirtyGroups.push_back(groupId);
        }
    }

    void BatchManager::generate(Scene* scene, const std::vector<int>& groupIds)
    {
        if (groupIds.empty()) {
            return;
        }
        // Remembered so updateAll() can regenerate a dirty group on its own. Only a
        // real prepare/generate sets it, which is what stops a group registered
        // during setup from being built before the app has tagged anything.
        if (scene) {
            _scene = scene;
        }
        std::erase_if(_dirtyGroups, [&groupIds](const int id) {
            return std::find(groupIds.begin(), groupIds.end(), id) != groupIds.end();
        });

        const auto inScope = [&groupIds](const int groupId) {
            return std::find(groupIds.begin(), groupIds.end(), groupId) != groupIds.end();
        };

        // 1. Destroy only these groups' batches; every other group keeps its own.
        destroyGroups(scene, groupIds);

        // 2. Collect mesh instances by (groupId, material pointer).
        //    Key: (batchGroupId << 32) | material pointer hash — but simpler to use a nested map.
        struct MaterialKey {
            int groupId;
            Material* material;
            bool operator==(const MaterialKey& o) const { return groupId == o.groupId && material == o.material; }
        };
        struct MaterialKeyHash {
            size_t operator()(const MaterialKey& k) const {
                auto h1 = std::hash<int>{}(k.groupId);
                auto h2 = std::hash<void*>{}(static_cast<void*>(k.material));
                return h1 ^ (h2 << 1);
            }
        };

        std::unordered_map<MaterialKey, std::vector<MeshInstance*>, MaterialKeyHash> groups;

        for (auto* rc : RenderComponent::instances()) {
            if (!rc || !rc->enabled()) continue;

            const int groupId = rc->batchGroupId();
            if (groupId < 0) continue;                     // Not tagged for batching.
            if (_groups.find(groupId) == _groups.end()) continue;  // Unknown group.
            if (!inScope(groupId)) continue;               // Another group's turn.

            // Upstream's whole-entity rule: an entity with any skinned or morphed
            // mesh instance contributes none of them. Merging bakes each source's
            // world transform into a shared buffer, so a deforming mesh loses the
            // thing that makes it deform. A SKINNED one is rejected anyway further
            // down by its 88-byte stride, but a MORPHED one carries the ordinary
            // packed layout with its deltas in a separate buffer — it would merge
            // cleanly and then simply stop moving, with nothing reporting why.
            std::vector<bool> deforms;
            deforms.reserve(rc->meshInstances().size());
            for (auto* mi : rc->meshInstances()) {
                deforms.push_back(mi && (mi->skinInstance() || mi->morphInstance()));
            }
            if (!entityIsBatchable(deforms)) {
                spdlog::warn("[BatchManager] Skipping a render component in batch group {}: "
                             "a skinned or morphed mesh instance cannot be merged, and "
                             "upstream excludes the whole entity when any of its "
                             "instances deforms", groupId);
                continue;
            }

            for (auto* mi : rc->meshInstances()) {
                if (!mi || !mi->mesh() || !mi->mesh()->getVertexBuffer()) continue;

                MaterialKey key{groupId, mi->material()};
                groups[key].push_back(mi);
            }
        }

        // 3. Build batches. A (group, material) bucket is not one batch: mesh
        //    instances that disagree about their vertex layout, primitive type or
        //    shadow flags cannot share a draw, and a batch may not grow past the
        //    group's maxAabbSize. splitBatchLists applies those rules and hands back
        //    one list per batch.
        int batchCount = 0;
        int totalOrigMeshInstances = 0;
        for (auto& [key, meshInstances] : groups) {
            if (meshInstances.size() < 2) continue;  // No point batching a single mesh.

            const auto* group = getGroupById(key.groupId);
            const bool dynamic = group && group->dynamic;

            std::vector<BatchCandidate> candidates;
            candidates.reserve(meshInstances.size());
            for (auto* mi : meshInstances) {
                candidates.push_back(describeCandidate(mi));
            }

            const auto lists = splitBatchLists(candidates,
                group ? group->maxAabbSize : 0.0f, dynamic);

            for (const auto& indices : lists) {
                if (indices.size() < 2) continue;  // A batch of one saves nothing.

                std::vector<MeshInstance*> listInstances;
                listInstances.reserve(indices.size());
                for (const size_t index : indices) {
                    listInstances.push_back(meshInstances[index]);
                }

                std::unique_ptr<Batch> batch = dynamic
                    ? createDynamicBatch(listInstances, key.groupId)
                    : createBatch(listInstances, key.groupId);
                if (!batch) {
                    continue;
                }

                totalOrigMeshInstances += static_cast<int>(listInstances.size());
                batchCount++;

                // Register batch MeshInstance with scene layers.
                if (scene && scene->layers()) {
                    if (group && !group->layers.empty()) {
                        for (int layerId : group->layers) {
                            auto layer = scene->layers()->getLayerById(layerId);
                            if (layer) {
                                layer->addMeshInstances({batch->meshInstance.get()});
                            }
                        }
                    } else {
                        // Default: add to WORLD layer (id=1).
                        auto worldLayer = scene->layers()->getLayerById(1);
                        if (worldLayer) {
                            worldLayer->addMeshInstances({batch->meshInstance.get()});
                        }
                    }
                }

                _batchMeshInstances.push_back(batch->meshInstance.get());
                _batches.push_back(std::move(batch));
            }
        }

        if (batchCount > 0) {
            spdlog::info("[BatchManager] Created {} batches from {} mesh instances",
                         batchCount, totalOrigMeshInstances);
        }
    }

    // -------------------------------------------------------------------------
    // destroy() — 
    // -------------------------------------------------------------------------
    void BatchManager::destroy(Scene* scene)
    {
        destroyGroups(scene, {});
    }

    void BatchManager::destroyGroups(Scene* scene, const std::vector<int>& groupIds)
    {
        // An empty list means every group — that is what destroy() asks for.
        const auto inScope = [&groupIds](const int groupId) {
            return groupIds.empty() ||
                std::find(groupIds.begin(), groupIds.end(), groupId) != groupIds.end();
        };

        for (auto& batch : _batches) {
            if (!inScope(batch->batchGroupId)) {
                continue;
            }
            // Remove batch MeshInstance from layers.
            if (scene && scene->layers() && batch->meshInstance) {
                const auto* group = getGroupById(batch->batchGroupId);
                if (group && !group->layers.empty()) {
                    for (int layerId : group->layers) {
                        auto layer = scene->layers()->getLayerById(layerId);
                        if (layer) {
                            layer->removeMeshInstances({batch->meshInstance.get()});
                        }
                    }
                } else {
                    auto worldLayer = scene->layers()->getLayerById(1);
                    if (worldLayer) {
                        worldLayer->removeMeshInstances({batch->meshInstance.get()});
                    }
                }
            }

            if (batch->meshInstance) {
                _batchMeshInstances.erase(
                    std::remove(_batchMeshInstances.begin(), _batchMeshInstances.end(),
                                batch->meshInstance.get()),
                    _batchMeshInstances.end());
            }

            // Restore visibility of original mesh instances.
            for (auto* mi : batch->origMeshInstances) {
                mi->setVisible(true);
            }
        }
        std::erase_if(_batches, [&inScope](const std::unique_ptr<Batch>& batch) {
            return batch && inScope(batch->batchGroupId);
        });
    }

    // -------------------------------------------------------------------------
    // createBatch() — 
    // -------------------------------------------------------------------------
    std::unique_ptr<Batch> BatchManager::createBatch(
        const std::vector<MeshInstance*>& meshInstances, int batchGroupId)
    {
        if (meshInstances.empty() || !_device) return nullptr;
        if (!listIsMergeable(meshInstances)) return nullptr;

        // --- 1. Count total vertices and indices. ---
        int totalVertices = 0;
        int totalIndices = 0;
        for (auto* mi : meshInstances) {
            auto vb = mi->mesh()->getVertexBuffer();
            auto ib = mi->mesh()->getIndexBuffer();
            if (!vb) continue;

            totalVertices += vb->numVertices();
            if (ib) {
                totalIndices += ib->numIndices();
            } else {
                // Non-indexed: treat vertex count as index count (identity indices).
                totalIndices += vb->numVertices();
            }
        }

        if (totalVertices == 0) return nullptr;

        // --- 2. Allocate merged buffers. ---
        std::vector<PackedVertex> mergedVertices;
        mergedVertices.reserve(totalVertices);

        std::vector<uint32_t> mergedIndices;
        mergedIndices.reserve(totalIndices);

        BoundingBox mergedAabb;
        bool aabbInitialized = false;

        uint32_t vertexOffset = 0;

        // --- 3. Merge geometry. ---
        for (auto* mi : meshInstances) {
            auto vb = mi->mesh()->getVertexBuffer();
            auto ib = mi->mesh()->getIndexBuffer();
            if (!vb || vb->storage().empty()) continue;

            const int vertCount = vb->numVertices();
            const auto* srcVerts = reinterpret_cast<const PackedVertex*>(vb->storage().data());

            // Get world transform.
            Matrix4 worldMatrix = Matrix4::identity();
            if (mi->node()) {
                worldMatrix = mi->node()->worldTransform();
            }

            // Compute normal matrix (inverse-transpose of upper-left 3x3).
            // For uniform-scale transforms, the normal matrix == the rotation part.
            // For non-uniform scale, we need the full inverse-transpose.
            Matrix4 normalMatrix = worldMatrix.inverse().transpose();

            // Transform vertices.
            for (int i = 0; i < vertCount; i++) {
                PackedVertex v = srcVerts[i];

                // Transform position by world matrix.
                Vector3 pos = worldMatrix.transformPoint(Vector3(v.px, v.py, v.pz));
                v.px = pos.getX(); v.py = pos.getY(); v.pz = pos.getZ();

                // Transform normal by normal matrix (3x3 part only, no translation).
                float nnx = normalMatrix.getElement(0, 0) * v.nx +
                            normalMatrix.getElement(1, 0) * v.ny +
                            normalMatrix.getElement(2, 0) * v.nz;
                float nny = normalMatrix.getElement(0, 1) * v.nx +
                            normalMatrix.getElement(1, 1) * v.ny +
                            normalMatrix.getElement(2, 1) * v.nz;
                float nnz = normalMatrix.getElement(0, 2) * v.nx +
                            normalMatrix.getElement(1, 2) * v.ny +
                            normalMatrix.getElement(2, 2) * v.nz;
                Vector3 transformedNormal = Vector3(nnx, nny, nnz).normalized();
                v.nx = transformedNormal.getX(); v.ny = transformedNormal.getY(); v.nz = transformedNormal.getZ();

                // Transform tangent.xyz by normal matrix (3x3 part only), preserve w (handedness).
                float ttx = normalMatrix.getElement(0, 0) * v.tx +
                            normalMatrix.getElement(1, 0) * v.ty +
                            normalMatrix.getElement(2, 0) * v.tz;
                float tty = normalMatrix.getElement(0, 1) * v.tx +
                            normalMatrix.getElement(1, 1) * v.ty +
                            normalMatrix.getElement(2, 1) * v.tz;
                float ttz = normalMatrix.getElement(0, 2) * v.tx +
                            normalMatrix.getElement(1, 2) * v.ty +
                            normalMatrix.getElement(2, 2) * v.tz;
                Vector3 transformedTangent = Vector3(ttx, tty, ttz).normalized();
                v.tx = transformedTangent.getX(); v.ty = transformedTangent.getY(); v.tz = transformedTangent.getZ();
                // v.tw (handedness) is preserved unchanged.

                // UVs are unchanged.
                mergedVertices.push_back(v);
            }

            // Remap indices with vertex offset.
            if (ib && !ib->storage().empty()) {
                const int idxCount = ib->numIndices();
                const auto* idxData = ib->storage().data();

                if (ib->format() == INDEXFORMAT_UINT16) {
                    const auto* idx16 = reinterpret_cast<const uint16_t*>(idxData);
                    for (int i = 0; i < idxCount; i++) {
                        mergedIndices.push_back(static_cast<uint32_t>(idx16[i]) + vertexOffset);
                    }
                } else if (ib->format() == INDEXFORMAT_UINT32) {
                    const auto* idx32 = reinterpret_cast<const uint32_t*>(idxData);
                    for (int i = 0; i < idxCount; i++) {
                        mergedIndices.push_back(idx32[i] + vertexOffset);
                    }
                } else {
                    // UINT8
                    for (int i = 0; i < idxCount; i++) {
                        mergedIndices.push_back(static_cast<uint32_t>(idxData[i]) + vertexOffset);
                    }
                }
            } else {
                // Non-indexed: generate identity indices.
                for (int i = 0; i < vertCount; i++) {
                    mergedIndices.push_back(vertexOffset + static_cast<uint32_t>(i));
                }
            }

            // Merge AABB.
            BoundingBox instanceAabb = mi->aabb();
            if (!aabbInitialized) {
                mergedAabb = instanceAabb;
                aabbInitialized = true;
            } else {
                mergedAabb.add(instanceAabb);
            }

            vertexOffset += static_cast<uint32_t>(vertCount);
        }

        // --- 4. Create GPU buffers. ---
        const int mergedVertCount = static_cast<int>(mergedVertices.size());
        const int mergedIdxCount = static_cast<int>(mergedIndices.size());

        // Vertex buffer: same format as original (PackedVertex = 56 bytes).
        auto vertFormat = meshInstances[0]->mesh()->getVertexBuffer()->format();
        VertexBufferOptions vbOpts;
        vbOpts.data.resize(mergedVertCount * sizeof(PackedVertex));
        std::memcpy(vbOpts.data.data(), mergedVertices.data(), vbOpts.data.size());

        auto mergedVB = _device->createVertexBuffer(vertFormat, mergedVertCount, vbOpts);
        if (!mergedVB) {
            spdlog::warn("[BatchManager] Failed to create merged vertex buffer ({} verts)", mergedVertCount);
            return nullptr;
        }
        mergedVB->unlock();

        // Index buffer: always UINT32 for merged geometry (may exceed 65535 vertices).
        std::vector<uint8_t> idxData(mergedIdxCount * sizeof(uint32_t));
        std::memcpy(idxData.data(), mergedIndices.data(), idxData.size());

        auto mergedIB = _device->createIndexBuffer(INDEXFORMAT_UINT32, mergedIdxCount, idxData);
        if (!mergedIB) {
            spdlog::warn("[BatchManager] Failed to create merged index buffer ({} indices)", mergedIdxCount);
            return nullptr;
        }

        // --- 5. Create Mesh. ---
        auto mergedMesh = std::make_shared<Mesh>();
        mergedMesh->setVertexBuffer(mergedVB);
        mergedMesh->setIndexBuffer(mergedIB);
        mergedMesh->setAabb(mergedAabb);

        Primitive prim;
        prim.type = PRIMITIVE_TRIANGLES;
        prim.base = 0;
        prim.count = mergedIdxCount;
        prim.indexed = true;
        mergedMesh->setPrimitive(prim);

        // --- 6. Create MeshInstance. ---
        auto batch = std::make_unique<Batch>();
        batch->batchGroupId = batchGroupId;
        batch->mesh = mergedMesh;
        batch->vertexBuffer = mergedVB;
        batch->indexBuffer = mergedIB;

        // Use identity transform — geometry is already in world space.
        batch->node.setPosition(Vector3(0, 0, 0));
        // The batch node is a standalone root with no parent, so its hierarchy-enabled
        // flag would stay false and the shadow caster filter (which tests
        // node()->enabled()) would drop the batch.
        batch->node.setEnabledInHierarchy(true);

        Material* sharedMaterial = meshInstances[0]->material();
        batch->meshInstance = std::make_unique<MeshInstance>(
            mergedMesh.get(), sharedMaterial, &batch->node);

        // Inherit shadow flags from first original.
        batch->meshInstance->setCastShadow(meshInstances[0]->castShadow());
        batch->meshInstance->setReceiveShadow(meshInstances[0]->receiveShadow());

        // Hide original mesh instances.
        for (auto* mi : meshInstances) {
            mi->setVisible(false);
            batch->origMeshInstances.push_back(mi);
        }

        return batch;
    }
    // -------------------------------------------------------------------------
    // updateAll() — 
    // -------------------------------------------------------------------------
    void BatchManager::updateAll()
    {
        // Rebuild anything marked dirty since the last frame, before the per-frame
        // refresh below reads it. Nothing happens until the app has prepared once:
        // _scene is null until then, and generate needs it to register the batch
        // mesh instances with the group's layers.
        if (!_dirtyGroups.empty() && _scene) {
            const std::vector<int> dirty = _dirtyGroups;
            generate(_scene, dirty);
        }

        for (auto& batch : _batches) {
            if (!batch->dynamic) continue;
            if (batch->skinBatchInstance) {
                batch->skinBatchInstance->updateMatrices();
            }
            batch->updateBoundingBox();
        }
    }

    // -------------------------------------------------------------------------
    // createDynamicBatch() — (dynamic path)
    // Uses Metal buffer (slot 6) for bone data.
    // -------------------------------------------------------------------------
    std::unique_ptr<Batch> BatchManager::createDynamicBatch(
        const std::vector<MeshInstance*>& meshInstances, int batchGroupId)
    {
        if (meshInstances.empty() || !_device) return nullptr;
        if (!listIsMergeable(meshInstances)) return nullptr;

        // --- 1. Count total vertices and indices. ---
        int totalVertices = 0;
        int totalIndices = 0;
        for (auto* mi : meshInstances) {
            auto vb = mi->mesh()->getVertexBuffer();
            auto ib = mi->mesh()->getIndexBuffer();
            if (!vb) continue;

            totalVertices += vb->numVertices();
            if (ib) {
                totalIndices += ib->numIndices();
            } else {
                totalIndices += vb->numVertices();
            }
        }

        if (totalVertices == 0) return nullptr;

        // --- 2. Allocate merged buffers (DynamicBatchVertex = 60 bytes). ---
        std::vector<DynamicBatchVertex> mergedVertices;
        mergedVertices.reserve(totalVertices);

        std::vector<uint32_t> mergedIndices;
        mergedIndices.reserve(totalIndices);

        // Collect node pointers for SkinBatchInstance.
        std::vector<GraphNode*> boneNodes;
        boneNodes.reserve(meshInstances.size());

        uint32_t vertexOffset = 0;

        // --- 3. Merge geometry (local space — no world transform). ---
        for (int instIdx = 0; instIdx < static_cast<int>(meshInstances.size()); ++instIdx) {
            auto* mi = meshInstances[instIdx];
            auto vb = mi->mesh()->getVertexBuffer();
            auto ib = mi->mesh()->getIndexBuffer();
            if (!vb || vb->storage().empty()) continue;

            const int vertCount = vb->numVertices();
            const auto* srcVerts = reinterpret_cast<const PackedVertex*>(vb->storage().data());

            // Copy vertices in local space (no world transform) + set bone index.
            for (int i = 0; i < vertCount; i++) {
                const PackedVertex& sv = srcVerts[i];
                DynamicBatchVertex dv;
                dv.px = sv.px; dv.py = sv.py; dv.pz = sv.pz;
                dv.nx = sv.nx; dv.ny = sv.ny; dv.nz = sv.nz;
                dv.u = sv.u;   dv.v = sv.v;
                dv.tx = sv.tx; dv.ty = sv.ty; dv.tz = sv.tz; dv.tw = sv.tw;
                dv.u1 = sv.u1; dv.v1 = sv.v1;
                dv.boneIndex = static_cast<float>(instIdx);
                mergedVertices.push_back(dv);
            }

            // Remap indices with vertex offset.
            if (ib && !ib->storage().empty()) {
                const int idxCount = ib->numIndices();
                const auto* idxData = ib->storage().data();

                if (ib->format() == INDEXFORMAT_UINT16) {
                    const auto* idx16 = reinterpret_cast<const uint16_t*>(idxData);
                    for (int i = 0; i < idxCount; i++) {
                        mergedIndices.push_back(static_cast<uint32_t>(idx16[i]) + vertexOffset);
                    }
                } else if (ib->format() == INDEXFORMAT_UINT32) {
                    const auto* idx32 = reinterpret_cast<const uint32_t*>(idxData);
                    for (int i = 0; i < idxCount; i++) {
                        mergedIndices.push_back(idx32[i] + vertexOffset);
                    }
                } else {
                    for (int i = 0; i < idxCount; i++) {
                        mergedIndices.push_back(static_cast<uint32_t>(idxData[i]) + vertexOffset);
                    }
                }
            } else {
                for (int i = 0; i < vertCount; i++) {
                    mergedIndices.push_back(vertexOffset + static_cast<uint32_t>(i));
                }
            }

            // Collect node for matrix palette.
            boneNodes.push_back(mi->node());

            vertexOffset += static_cast<uint32_t>(vertCount);
        }

        // --- 4. Create GPU buffers (DynamicBatchVertex = 60 bytes). ---
        const int mergedVertCount = static_cast<int>(mergedVertices.size());
        const int mergedIdxCount = static_cast<int>(mergedIndices.size());

        // Build vertex format for dynamic batch (60 bytes stride).
        auto dynamicElements = VertexFormat::standardElements();
        dynamicElements.push_back(
            {VertexSemantic::SEMANTIC_ATTR5, VertexDataType::TYPE_FLOAT32, 1, 56});
        auto dynamicFormat = std::make_shared<VertexFormat>(
            static_cast<int>(sizeof(DynamicBatchVertex)), std::move(dynamicElements));

        VertexBufferOptions vbOpts;
        vbOpts.data.resize(mergedVertCount * sizeof(DynamicBatchVertex));
        std::memcpy(vbOpts.data.data(), mergedVertices.data(), vbOpts.data.size());

        auto mergedVB = _device->createVertexBuffer(dynamicFormat, mergedVertCount, vbOpts);
        if (!mergedVB) {
            spdlog::warn("[BatchManager] Failed to create dynamic batch vertex buffer ({} verts)", mergedVertCount);
            return nullptr;
        }
        mergedVB->unlock();

        // Index buffer: always UINT32.
        std::vector<uint8_t> idxData(mergedIdxCount * sizeof(uint32_t));
        std::memcpy(idxData.data(), mergedIndices.data(), idxData.size());

        auto mergedIB = _device->createIndexBuffer(INDEXFORMAT_UINT32, mergedIdxCount, idxData);
        if (!mergedIB) {
            spdlog::warn("[BatchManager] Failed to create dynamic batch index buffer ({} indices)", mergedIdxCount);
            return nullptr;
        }

        // --- 5. Create Mesh. ---
        auto mergedMesh = std::make_shared<Mesh>();
        mergedMesh->setVertexBuffer(mergedVB);
        mergedMesh->setIndexBuffer(mergedIB);

        Primitive prim;
        prim.type = PRIMITIVE_TRIANGLES;
        prim.base = 0;
        prim.count = mergedIdxCount;
        prim.indexed = true;
        mergedMesh->setPrimitive(prim);

        // --- 6. Create Batch + MeshInstance + SkinBatchInstance. ---
        auto batch = std::make_unique<Batch>();
        batch->batchGroupId = batchGroupId;
        batch->dynamic = true;
        batch->mesh = mergedMesh;
        batch->vertexBuffer = mergedVB;
        batch->indexBuffer = mergedIB;

        // Identity transform — per-instance transforms come from palette.
        batch->node.setPosition(Vector3(0, 0, 0));
        // Standalone root node — see createBatch().
        batch->node.setEnabledInHierarchy(true);

        Material* sharedMaterial = meshInstances[0]->material();
        batch->meshInstance = std::make_unique<MeshInstance>(
            mergedMesh.get(), sharedMaterial, &batch->node);

        // Mark the batch MeshInstance as a dynamic batch for shader variant selection.
        batch->meshInstance->setDynamicBatch(true);

        // Inherit shadow flags from first original.
        batch->meshInstance->setCastShadow(meshInstances[0]->castShadow());
        batch->meshInstance->setReceiveShadow(meshInstances[0]->receiveShadow());

        // Create SkinBatchInstance with node pointers.
        batch->skinBatchInstance = std::make_unique<SkinBatchInstance>(std::move(boneNodes));
        batch->meshInstance->setSkinBatchInstance(batch->skinBatchInstance.get());

        // Initial matrix update + AABB.
        batch->skinBatchInstance->updateMatrices();
        batch->updateBoundingBox();

        // Hide original mesh instances.
        for (auto* mi : meshInstances) {
            mi->setVisible(false);
            batch->origMeshInstances.push_back(mi);
        }

        spdlog::trace("[BatchManager] Dynamic batch: {} instances, {} verts, {} indices, {} bones",
            meshInstances.size(), mergedVertCount, mergedIdxCount, boneNodes.size());

        return batch;
    }

} // visutwin::canvas
