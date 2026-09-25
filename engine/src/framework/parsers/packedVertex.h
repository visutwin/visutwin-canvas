// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The parsers' interleaved static vertex: position, normal, uv0, tangent (+ handedness)
// and uv1, 14 floats. ONE definition: the glTF, OBJ, STL and Assimp parsers write it and
// BatchManager merges by reinterpreting a source vertex buffer as it
// (formatIsPackedVertexLayout in batchSplit.h is the guard on that cast). Five private
// copies of it used to have to agree by convention.
//
#pragma once

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
}
