// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.03.2026.
//
//
//
#pragma once

#include <memory>
#include <vector>

#include "batchGroup.h"
#include "skinBatchInstance.h"
#include "scene/graphNode.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "core/shape/boundingBox.h"
#include "platform/graphics/indexBuffer.h"
#include "platform/graphics/vertexBuffer.h"

namespace visutwin::canvas
{
    /**
     * A Batch is the output of BatchManager::createBatch(). It holds the combined mesh
     * that replaces multiple original MeshInstances sharing the same material.
     *
     * The originals are hidden (visible=false) while the batch is alive; calling
     * BatchManager::destroy() restores them.
     *
     * For dynamic batches, the SkinBatchInstance provides a per-frame matrix palette
     * and updateBoundingBox() refreshes the AABB from the originals.
     */
    class Batch
    {
    public:
        /** The batch group this batch belongs to. */
        int batchGroupId = BatchGroup::NOID;

        /** True for dynamic batches (transforms change per frame). */
        bool dynamic = false;

        /** The original mesh instances that were merged into this batch. */
        std::vector<MeshInstance*> origMeshInstances;

        /** The combined mesh instance that replaces the originals. */
        std::unique_ptr<MeshInstance> meshInstance;

        /** SkinBatchInstance for dynamic batches (owns the matrix palette).
         *  nullptr for static batches. */
        std::unique_ptr<SkinBatchInstance> skinBatchInstance;

        /** Owned resources — kept alive as long as the batch exists. */
        std::shared_ptr<Mesh> mesh;
        std::shared_ptr<VertexBuffer> vertexBuffer;
        std::shared_ptr<IndexBuffer> indexBuffer;

        /** Identity-transform node for the combined mesh (geometry is in world space
         *  for static batches, or local space for dynamic batches). */
        GraphNode node;

        /** Update AABB from all original mesh instances (dynamic batches only).
         *  */
        void updateBoundingBox()
        {
            if (origMeshInstances.empty() || !meshInstance) return;
            BoundingBox aabb = origMeshInstances[0]->aabb();
            for (size_t i = 1; i < origMeshInstances.size(); ++i) {
                aabb.add(origMeshInstances[i]->aabb());
            }
            meshInstance->setCustomAabb(aabb);
        }
    };
}
