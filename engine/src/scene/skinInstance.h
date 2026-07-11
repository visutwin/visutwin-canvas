// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 22.12.2025.
//
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <core/math/matrix4.h>

namespace visutwin::canvas
{
    class GraphNode;
    class Skin;

    /**
     * A skin instance is responsible for generating the matrix palette that is used to skin vertices
     * from object space to world space.
     *
     * The palette entry for bone i is: inverse(rootNode.worldTransform) * bone[i].worldTransform *
     * skin.inverseBindPose[i]. Keeping the palette relative to the mesh instance's node (rootNode)
     * preserves float precision at large world coordinates; the vertex shader applies the node's
     * model matrix on top, so the two cancel and vertices land in world space.
     *
     * DEVIATION: the palette is uploaded per draw as a float4x4 Metal buffer at slot 6 (the same
     * ring-buffer path dynamic batching uses) instead of upstream's RGBA32F bone texture.
     */
    class SkinInstance
    {
    public:
        explicit SkinInstance(std::shared_ptr<Skin> skin);

        const std::shared_ptr<Skin>& skin() const { return _skin; }

        /** Resolved bone graph nodes, one per skin.inverseBindPose entry. */
        void setBones(std::vector<GraphNode*> bones);
        const std::vector<GraphNode*>& bones() const { return _bones; }

        void setRootBone(GraphNode* rootBone) { _rootBone = rootBone; }
        GraphNode* rootBone() const { return _rootBone; }

        /**
         * Recompute the matrix palette for this frame. No-ops if already computed for the
         * current frame index (a SkinInstance is shared by several mesh instances and hit by
         * multiple passes — shadow cascades and forward — within one frame).
         */
        void updateMatrixPalette(GraphNode* rootNode);

        const void* paletteData() const { return _palette.data(); }
        size_t paletteSizeBytes() const { return _palette.size() * sizeof(Matrix4); }
        int boneCount() const { return static_cast<int>(_palette.size()); }

        /**
         * Advance the global skin frame index. Called once per frame by the renderer
         * (single-threaded render loop) so updateMatrixPalette() can dedupe its work.
         */
        static void beginFrame() { ++s_frameIndex; }

    private:
        std::shared_ptr<Skin> _skin;
        std::vector<GraphNode*> _bones;
        GraphNode* _rootBone = nullptr;

        // One float4x4 per bone, column-major — uploaded directly as `constant float4x4 *palette`.
        std::vector<Matrix4> _palette;

        // Sequential index of when the palette update was performed the last time.
        uint64_t _skinUpdateIndex = ~0ull;

        static uint64_t s_frameIndex;
    };
}
