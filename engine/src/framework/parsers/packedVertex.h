// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 25.09.2026
//
// The parsers' interleaved static vertex: position, normal, uv0, tangent (+ handedness)
// and uv1, 14 floats. ONE definition: the glTF, OBJ, STL and Assimp parsers write it and
// BatchManager merges by reinterpreting a source vertex buffer as it
// (formatIsPackedVertexLayout in batchSplit.h is the guard on that cast), so the writers
// and the merge cannot disagree about it.
//
// The tangent helpers the parsers share (generateTangents, tangentFromNormal) and the upload
// of a packed mesh (createPackedMesh) live here too, for the same reason.
//
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Mesh;
    struct Vector3;
    class VertexFormat;
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

    /**
     * The GPU mesh of packed vertices and a triangle list: a static vertex buffer in `format`
     * (a 56-byte PackedVertex layout), an index buffer of 16-bit indices when every vertex fits
     * and 32-bit otherwise, one indexed triangle-list primitive, and the bounds [minPos, maxPos].
     * Decoding, winding, normals, tangents and the bounds policy stay the parser's.
     */
    std::shared_ptr<Mesh> createPackedMesh(GraphicsDevice& device, const std::shared_ptr<VertexFormat>& format,
        const std::vector<PackedVertex>& vertices, const std::vector<uint32_t>& indices,
        const Vector3& minPos, const Vector3& maxPos);
}
