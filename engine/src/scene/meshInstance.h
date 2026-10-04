// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.12.2025
//
#pragma once

#include <cstdint>
#include <functional>
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
#include "platform/graphics/stencilParameters.h"
#include "platform/graphics/vertexBuffer.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class GSplatInstance;
    class ParticleEmitter;
    class InstanceCuller;
    class Camera;
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

        /// The shared ownership this instance holds of its mesh and material, or null
        /// when it only borrows them. A clone takes the same ownership, so it keeps
        /// rendering after the source (or the container it came from) is gone.
        const std::shared_ptr<Mesh>& meshShared() const { return _meshOwned; }
        const std::shared_ptr<Material>& materialShared() const { return _materialOwned; }

        /// A new instance of the same mesh and material on `node`, for Entity::clone.
        /// Carries the drawing settings (shadows, cull, mask, bucket, draw order,
        /// sort callback, batch group), the same shared ownership, and a NEW morph
        /// instance holding the same weights and a NEW skin instance on the same
        /// bones — the entity clone remaps those bones into the cloned subtree.
        /// Per-instance data that belongs to where the source sits is left out: the baked lightmap, instancing buffers, a custom
        /// (world-space) AABB and batch membership.
        [[nodiscard]] std::unique_ptr<MeshInstance> cloneFor(GraphNode* node) const;

        /**
         * Replaces the material of this instance,
         * e.g. to render a loaded model with a custom ShaderMaterial. Drops any
         * shared ownership taken from the source container — the caller owns the
         * new material and must keep it alive.
         */
        void setMaterial(Material* material)
        {
            _material = material;
            _materialOwned.reset();
        }

        /// Replaces the material and CO-OWNS it, as the shared-ownership constructor
        /// does: what a container's material variant uses, so the swapped-in material
        /// outlives the container too.
        void setMaterial(std::shared_ptr<Material> material)
        {
            _material = material.get();
            _materialOwned = std::move(material);
        }

        GraphNode* node() const { return _node; }

        bool castShadow() const { return _castShadow; }
        void setCastShadow(const bool value) { _castShadow = value; }

        //receiveShadow getter/setter.
        // When false, the SHADERDEF_NOSHADOW flag is set on the shader defs.
        bool receiveShadow() const { return _receiveShadow; }
        void setReceiveShadow(const bool value) { _receiveShadow = value; }

        // A lightmap of this mesh instance's own, which is what a lightmapper bakes.
        // It takes PRIORITY over the material's lightMap, and binds in the same slot, so meshes that
        // share one material each show their own bake (a bake written into the shared
        // material would show the last one on every such mesh).
        // Owned here: the texture must outlive every draw that samples it.
        const std::shared_ptr<Texture>& lightMap() const { return _lightMap; }
        void setLightMap(std::shared_ptr<Texture> texture) { _lightMap = std::move(texture); }

        bool cull() const { return _cull; }
        void setCull(const bool value) { _cull = value; }

        /**
         * The node's world transform already carries
         * the vertices into CLIP space (a screen-space UI element, through its screen's
         * projection), so the vertex stage skips the camera's view-projection
         * (VT_FEATURE_SCREEN_SPACE) and ANY camera that renders the layer draws it where
         * the screen put it. Such an instance has no world-space bounds to cull and casts
         * no shadow, so setting it turns both off; depth-only passes skip it.
         */
        bool screenSpace() const { return _screenSpace; }
        void setScreenSpace(const bool value)
        {
            _screenSpace = value;
            if (value) {
                _cull = false;
                _castShadow = false;
            }
        }

        bool visibleThisFrame() const { return _visibleThisFrame; }
        void setVisibleThisFrame(const bool value) { _visibleThisFrame = value; }

        /**
         * Drawn by the first forward pass of a frame that collects it, and skipped by every
         * later one: an overlay on a layer several cameras render (the performance HUD) is
         * drawn once, by the first camera to reach it, instead of once per camera. The gate
         * is at draw time because culling has already run by then.
         */
        bool drawOncePerFrame() const { return _drawOncePerFrame; }
        void setDrawOncePerFrame(const bool value) { _drawOncePerFrame = value; }

        /// True the first time it is asked in the frame `renderVersion`, false after that.
        /// Always true for an instance that is not drawOncePerFrame.
        bool claimDrawThisFrame(const int renderVersion)
        {
            if (!_drawOncePerFrame) {
                return true;
            }
            if (_drawnRenderVersion == renderVersion) {
                return false;
            }
            _drawnRenderVersion = renderVersion;
            return true;
        }

        /**
         * Application-authored order, used only by SORTMODE_MANUAL.
         * Lower draws first.
         */
        double drawOrder() const { return _drawOrder; }
        /// A double: an unmask draw sits BETWEEN two elements' orders.
        void setDrawOrder(const double value) { _drawOrder = value; }

        /// The stencil test and write this draw
        /// uses (a UI mask writes the stencil, the elements under it test it); null draws
        /// with the stencil off. Shared, so one parameter set can serve a whole mask level.
        const std::shared_ptr<StencilParameters>& stencilFront() const { return _stencilFront; }
        const std::shared_ptr<StencilParameters>& stencilBack() const { return _stencilBack; }
        void setStencil(std::shared_ptr<StencilParameters> front, std::shared_ptr<StencilParameters> back)
        {
            _stencilFront = std::move(front);
            _stencilBack = std::move(back);
        }

        /**
         * Coarse priority, the HIGHEST-priority field of the material sort key, so a
         * bucket is drawn entirely before the next whatever their materials are, and
         * the primary key of the two distance sorts too (back to front draws the
         * higher bucket first, front to back the lower; see distanceSortsBefore).
         * 8 bits. Default 127, the middle, so a mesh can be moved either side of
         * everything left at the default.
         *
         * This is what a caller reaches for when something must precede everything
         * else in its sublayer — a stencil mask, a depth primer — without splitting
         * it into its own layer.
         */
        static constexpr uint8_t kDefaultDrawBucket = 127;
        uint8_t drawBucket() const { return _drawBucket; }
        void setDrawBucket(const uint8_t value) { _drawBucket = value; }

        /**
         * Signed view-axis depth from the last sort (see sortDistance.h). Written by
         * the renderer before a distance-ordered or custom sort, so a custom
         * comparator can read it; meaningless in the other modes.
         */
        float sortDistance() const { return _sortDistance; }
        void setSortDistance(const float value) { _sortDistance = value; }

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
        // instancing (see Renderer::dispatchGpuInstanceCulling). Each camera that draws
        // the frame gets an output of its own, so every view draws what IT sees; a draw
        // for a camera that was not culled this frame (a picker, a bake) takes the whole
        // instance buffer.
        //
        // boundingSphereRadius is the per-instance bounding sphere radius in
        // local space — typically the mesh's own bounding sphere radius
        // multiplied by the largest instance scale, plus a safety margin.
        //
        // Re-call this method if the source instance count changes — each camera's
        // compacted buffer is sized for the count at enable time.
        void enableGpuInstanceCulling(GraphicsDevice* device, float boundingSphereRadius);

        bool gpuCullingEnabled() const { return _gpuCullingEnabled; }
        /// How many live mesh instances have GPU culling on. The renderer's per-frame
        /// dispatch sweeps the whole scene for them, and skips the sweep at zero.
        static int gpuCulledInstanceCount() { return GpuCullingCount::live; }
        float instanceCullRadius() const { return _instanceCullRadius; }

        /// One camera's GPU-culled view of the instances: the culler, whose indirect
        /// arguments the draw reads, and its compacted visible set wrapped for slot 5.
        struct GpuCullOutput
        {
            const Camera* camera = nullptr;
            std::unique_ptr<InstanceCuller> culler;
            std::shared_ptr<VertexBuffer> compacted;
            // GraphicsDevice::renderVersion() of the frame it was last culled in.
            int culledVersion = -1;
        };

        /// The output for `camera`, created on first use; null when the device cannot
        /// make one. A pointer into a list a later call may grow: use it at once.
        GpuCullOutput* gpuCullOutputFor(const Camera* camera);

        /// The output culled for `camera` in frame `renderVersion`, or null: the draw then
        /// takes the whole instance buffer.
        const GpuCullOutput* culledOutput(const Camera* camera, int renderVersion) const;

        /// Drops the outputs not culled since frame `oldestVersion` (a camera gone, or no
        /// longer drawing). A camera address reused by a new camera only ever gets an
        /// output culled again for it before it is drawn.
        void pruneGpuCullOutputs(int oldestVersion);

        // --- GPU skinning ---

        /** Skin instance driving this mesh (shared across submeshes of one skinned node). */
        SkinInstance* skinInstance() const { return _skinInstance.get(); }

        /// Shared ownership of the skin, so a second MeshInstance can render the same
        /// mesh in the same animated pose (e.g. an x-ray duplicate of a character in
        /// another layer).
        const std::shared_ptr<SkinInstance>& skinInstanceShared() const { return _skinInstance; }

        /**
         * Attach a skin instance.
         *
         * Frustum culling stays ON when the skin carries per-bone AABBs (the GLB
         * parser computes them), since aabb() then unions the used bones' boxes
         * through their current transforms. It is disabled only for skins without
         * them — a bind-pose AABB is not valid under animation. See the `_cull`
         * assignment in setSkinInstance().
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
            // The local bounds grow by the morph's reach, so the cached ones are stale.
            _aabbVer = -1;
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

        /**
         * Optional override of the distance the forward pass sorts this instance on.
         * The default is the signed
         * depth of the world AABB centre along the camera forward vector; a particle
         * system or a large transparent sheet can supply something better.
         */
        using SortDistanceCallback =
            std::function<float(const MeshInstance&, const Vector3& cameraPosition, const Vector3& cameraForward)>;
        const SortDistanceCallback& calculateSortDistance() const { return _calculateSortDistance; }
        void setCalculateSortDistance(SortDistanceCallback callback) { _calculateSortDistance = std::move(callback); }

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
        // The union of the instances' bounds in the NODE's space, which _customAabb
        // points at while instancing sets the bounds (aabb() carries it to world).
        BoundingBox _instancingLocalAabb;
        SortDistanceCallback _calculateSortDistance;
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

        // GPU instance culling: one output per camera that draws, made on the device
        // culling was enabled with.
        GraphicsDevice* _gpuCullDevice = nullptr;
        std::vector<GpuCullOutput> _gpuCullOutputs;
        float _instanceCullRadius = 0.0f;
        bool _gpuCullingEnabled = false;
        // Counts this instance in gpuCulledInstanceCount() for as long as it lives with
        // culling on. A member rather than a destructor, so the class keeps its implicit
        // ones; never copied or moved, like the instance that owns it.
        struct GpuCullingCount
        {
            inline static int live = 0;
            bool counted = false;
            GpuCullingCount() = default;
            GpuCullingCount(const GpuCullingCount&) = delete;
            GpuCullingCount& operator=(const GpuCullingCount&) = delete;
            ~GpuCullingCount() { if (counted) { --live; } }
            void count() { if (!counted) { counted = true; ++live; } }
        };
        GpuCullingCount _gpuCullingCount;

        bool _castShadow = true;
        bool _receiveShadow = true;
        std::shared_ptr<Texture> _lightMap;
        bool _cull = true;
        bool _screenSpace = false;
        bool _visibleThisFrame = false;
        bool _drawOncePerFrame = false;
        // renderVersion starts at 0 and only grows, so -1 is never a frame drawn.
        int _drawnRenderVersion = -1;
        double _drawOrder = 0.0;
        std::shared_ptr<StencilParameters> _stencilFront;
        std::shared_ptr<StencilParameters> _stencilBack;
        uint8_t _drawBucket = kDefaultDrawBucket;
        float _sortDistance = 0.0f;
        uint32_t _mask = MASK_AFFECT_DYNAMIC;

        // Batching
        int _batchGroupId = -1;   // BatchGroup::NOID
        bool _visible = true;     // Hidden when merged into a batch
        bool _dynamicBatch = false;  // True when part of a dynamic batch
    };
}
