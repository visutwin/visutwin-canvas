// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <core/math/vector3.h>

namespace visutwin::canvas
{
    class GraphicsDevice;
    class VertexBuffer;

    /**
     * A morph target (a.k.a. blend shape): per-vertex position (and optional normal)
     * deltas relative to the base mesh, blended in by weight.
     */
    class MorphTarget
    {
    public:
        std::string name;

        /** xyz per vertex; size = vertexCount * 3. */
        std::vector<float> deltaPositions;

        /** xyz per vertex; empty when the target has no normal deltas. */
        std::vector<float> deltaNormals;
    };

    /**
     * Set of morph targets for a mesh, shared by all MorphInstance objects created from it.
     *
     * DEVIATION: upstream accumulates active targets into RGBA textures via a render pass
     * each frame. This port packs all target deltas into one static GPU buffer
     * (per target, per vertex: float4 posDelta + float4 nrmDelta) bound at vertex buffer
     * slot 9; the vertex shader sums the active targets directly, driven by a small
     * per-draw MorphParams uniform (slot 10). No per-frame GPU pass is needed.
     */
    class Morph
    {
    public:
        Morph(std::vector<MorphTarget> targets, int vertexCount, GraphicsDevice* device);

        int targetCount() const { return static_cast<int>(_targets.size()); }
        int vertexCount() const { return _vertexCount; }
        const std::vector<MorphTarget>& targets() const { return _targets; }

        /** Packed delta buffer: targetCount * vertexCount pairs of float4 (pos, normal). */
        const std::shared_ptr<VertexBuffer>& deltaBuffer() const { return _deltaBuffer; }

    private:
        std::vector<MorphTarget> _targets;
        int _vertexCount = 0;
        std::shared_ptr<VertexBuffer> _deltaBuffer;
    };
}
