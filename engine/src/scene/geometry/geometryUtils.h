// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 15.09.2026
//
// Per-vertex tangent frames from positions, normals, UVs and a triangle list —
// Lengyel's method: accumulate each triangle's dP/du and dP/dv on its corners,
// Gram-Schmidt dP/du against the normal, and take the handedness from dP/dv.
//
// DEVIATION: the handedness is the sign that makes cross(n, t) * w point toward
// DECREASING v — toward v = 0, the image's top row. Upstream's calculateTangents
// points it toward increasing v, but nothing upstream renders a primitive with it:
// its primitive cache builds meshes without tangents, and its derivative TBN
// negates the dP/dv axis, which is the frame used here. A normal map's green
// channel is image-UP, and with a top-origin texture that is -v.
//
// DEVIATION: a tangent that vanishes after orthogonalisation (a corner with no
// usable UV gradient) falls back to an axis perpendicular to the normal instead of
// normalising a zero vector into NaN.
//
#pragma once

#include <cstdint>
#include <vector>

namespace visutwin::canvas
{
    /// Smooth normals as xyz per vertex: each triangle's unit face normal summed on its corners,
    /// then normalised, so every face counts the same whatever its area. A vertex used by no
    /// triangle, or only by degenerate ones, keeps a zero normal rather than NaN. positions are
    /// xyz per vertex, indices a triangle list.
    std::vector<float> calculateNormals(const std::vector<float>& positions, const std::vector<uint32_t>& indices);

    /// Tangents as xyzw per vertex (`bitangent = cross(n, t.xyz) * t.w`).
    /// positions and normals are xyz per vertex, uvs uv per vertex, indices a
    /// triangle list.
    std::vector<float> calculateTangents(const std::vector<float>& positions,
        const std::vector<float>& normals, const std::vector<float>& uvs,
        const std::vector<uint32_t>& indices);
}
