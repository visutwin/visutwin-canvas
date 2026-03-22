// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025.
//
//
//
#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "batch.h"
#include "batchGroup.h"
#include "scene/layer.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Scene;

    /**
     * Glues many mesh instances into a single one for better performance.
     *
     * BatchManager merges mesh instances that share the same material into a single
     * combined Mesh with one draw call. This dramatically reduces CPU overhead in
     * scenes with many static objects.
     *
     * Usage:
     *   1. addGroup() — register a BatchGroup configuration.
     *   2. Tag RenderComponents with setBatchGroupId(groupId).
     *   3. prepare() — build batches (merges geometry, hides originals).
     *   4. destroy() — tear down batches, restore originals.
     *
     * Supports both static and dynamic batching:
     * - Static: geometry baked to world space once.
     * - Dynamic: geometry in local space + per-vertex bone index, with a per-frame
     *   matrix palette providing world transforms (SkinBatchInstance).
     */
    class BatchManager
    {
    public:
        explicit BatchManager(GraphicsDevice* device);

        /** Register a batch group configuration. */
        void addGroup(const BatchGroup& group);

        /** Remove a batch group and destroy its batches. */
        void removeGroup(int groupId);

        /**
         * Build batches for all registered groups.
         * Called once after scene setup (or when meshes change).
         * Collects MeshInstances by (groupId, material), merges geometry,
         * hides originals, and creates combined MeshInstances.
         *
         * If scene is provided, batched MeshInstances are automatically registered
         * with the appropriate layers. If nullptr, the caller must manually add
         * batch->meshInstance to the desired layers.
         */
        void prepare(Scene* scene = nullptr);

        /** Destroy all batches, restoring original mesh instances to visible.
         *  If scene is provided, batched MeshInstances are removed from layers. */
        void destroy(Scene* scene = nullptr);

        /** Per-frame update: refresh matrix palettes and AABBs for dynamic batches.
         *  Call from Engine::update() each frame.
         *  */
        void updateAll();

        const std::vector<std::unique_ptr<Batch>>& batches() const { return _batches; }

        /** Get a batch group by id. Returns nullptr if not found. */
        const BatchGroup* getGroupById(int groupId) const;

    private:
        /**
         * Create a single static batch from mesh instances sharing a material.
         * Transforms all vertex positions/normals to world space, merges
         * index buffers with vertex offset remapping, creates a combined
         * Mesh + MeshInstance.
         */
        std::unique_ptr<Batch> createBatch(const std::vector<MeshInstance*>& meshInstances,
                                            int batchGroupId);

        /**
         * Create a single dynamic batch from mesh instances sharing a material.
         * Geometry stays in local space; each vertex gets a bone index that
         * maps to a per-frame matrix palette (SkinBatchInstance).
         * when dynamic=true.
         * Uses Metal buffer (slot 6) for bone data.
         */
        std::unique_ptr<Batch> createDynamicBatch(const std::vector<MeshInstance*>& meshInstances,
                                                   int batchGroupId);

        GraphicsDevice* _device;
        std::unordered_map<int, BatchGroup> _groups;
        std::vector<std::unique_ptr<Batch>> _batches;
    };
}
