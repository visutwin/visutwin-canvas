// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The parsers' interleaved static vertex: position, normal, uv0, tangent (+ handedness)
// and uv1, 14 floats. ONE definition: the glTF, OBJ, STL and Assimp parsers write it and
// BatchManager merges by reinterpreting a source vertex buffer as it
// (formatIsPackedVertexLayout in batchSplit.h is the guard on that cast), so the writers
// and the merge cannot disagree about it.
//
// The tangent helpers the parsers share (generateTangents, tangentFromNormal) live here too,
// for the same reason.
//
#pragma once

#include <cstdint>
#include <vector>

namespace visutwin::canvas
{
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
     * Per-vertex tangents from the UVs (Lengyel's accumulation), written into tx..tw, with the
     * handedness in tw. `indices` lists triangles; when empty, every three vertices are one.
     * A vertex whose UVs are degenerate takes a tangent perpendicular to its normal.
     *
     * NOTE the handedness: the bitangent cross(n, t) * w points toward INCREASING v here, the
     * opposite of `calculateTangents` (scene/geometry/geometryUtils.h), which the built-in
     * primitives use. The parsers flip V into the vertex, so this is the sign their meshes shade
     * with; do not merge the two functions without re-deriving that.
     */
    void generateTangents(std::vector<PackedVertex>& vertices, const std::vector<uint32_t>& indices);

    /// A tangent perpendicular to the normal, for a vertex with no UVs (handedness 1).
    void tangentFromNormal(float nx, float ny, float nz, float& tx, float& ty, float& tz, float& tw);
}
