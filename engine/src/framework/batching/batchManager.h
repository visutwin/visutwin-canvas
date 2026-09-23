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

        /**
         * Rebuild the batches of just these groups, leaving every other group's
         * batches in place. prepare() is this over all registered groups.
         *
         * A scene with several batch groups had no way to change one without paying
         * for all of them: prepare() destroys every batch, re-merges every group's
         * geometry and re-uploads it. Upstream regenerates per group for the same
         * reason.
         */
        void generate(Scene* scene, const std::vector<int>& groupIds);

        /**
         * Marks a group as needing its batches rebuilt. updateAll() picks it up on
         * the next frame, but only once prepare() has run at least once — before
         * that there is no scene to register batch mesh instances with, and the app
         * is still setting the group up.
         */
        void markGroupDirty(int groupId);

        /**
         * Some of a group's SOURCE mesh instances are about to go away — their render
         * component is being disabled, destroyed, moved to another group or is
         * replacing its mesh instances. A batch keeps raw pointers to its sources (and
         * a dynamic batch to their nodes, read every frame), so the group's batches are
         * destroyed NOW, while those pointers are still good, and the group is marked
         * dirty for updateAll() to rebuild from what remains. Upstream only marks the
         * group dirty (its remove()); garbage collection keeps its sources alive until
         * the rebuild, which C++ does not.
         */
        void sourcesLeaving(int groupId);

        /** Groups awaiting regeneration. Empty in the steady state. */
        const std::vector<int>& dirtyGroups() const { return _dirtyGroups; }

        /** Destroy all batches, restoring original mesh instances to visible.
         *  If scene is provided, batched MeshInstances are removed from layers. */
        void destroy(Scene* scene = nullptr);

        /** Per-frame update: refresh matrix palettes and AABBs for dynamic batches,
         *  and regenerate any group marked dirty.
         *  Call from Engine::update() each frame.
         *  */
        void updateAll();

        const std::vector<std::unique_ptr<Batch>>& batches() const { return _batches; }

        /**
         * Every live batch MeshInstance, across all BatchManagers.
         *
         * Batch mesh instances belong to no RenderComponent — they are registered
         * straight with the scene layers — so passes that sweep
         * RenderComponent::instances() (the shadow passes) would otherwise never see
         * them and batched geometry would cast no shadows. Maintained by
         * prepare()/destroy().
         */
        static const std::vector<MeshInstance*>& batchMeshInstances() { return _batchMeshInstances; }

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

        /// Destroys only the batches belonging to these groups; empty means all.
        void destroyGroups(Scene* scene, const std::vector<int>& groupIds);

        GraphicsDevice* _device;
        /// Remembered by prepare()/generate() so updateAll() can regenerate a dirty
        /// group. Null until the app has prepared once, which is what keeps a group
        /// added during setup from being built before the app asks.
        Scene* _scene = nullptr;
        std::vector<int> _dirtyGroups;
        std::unordered_map<int, BatchGroup> _groups;
        std::vector<std::unique_ptr<Batch>> _batches;

        static std::vector<MeshInstance*> _batchMeshInstances;
    };
}
