// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream geometry-utils calculateTangents. See the header for the two
// deviations.
//
#include "geometryUtils.h"

#include "core/math/vector3.h"

#include <cmath>
#include <cstddef>

namespace visutwin::canvas
{
    std::vector<float> calculateTangents(const std::vector<float>& positions,
        const std::vector<float>& normals, const std::vector<float>& uvs,
        const std::vector<uint32_t>& indices)
    {
        const size_t vertexCount = positions.size() / 3;
        std::vector<Vector3> tan1(vertexCount, Vector3(0.0f));
        std::vector<Vector3> tan2(vertexCount, Vector3(0.0f));

        for (size_t tri = 0; tri + 2 < indices.size(); tri += 3) {
            const uint32_t i1 = indices[tri];
            const uint32_t i2 = indices[tri + 1];
            const uint32_t i3 = indices[tri + 2];
            if (i1 >= vertexCount || i2 >= vertexCount || i3 >= vertexCount) {
                continue;
            }

            const Vector3 p1 = Vector3::load(&positions[i1 * 3]);
            const Vector3 e1 = Vector3::load(&positions[i2 * 3]) - p1;
            const Vector3 e2 = Vector3::load(&positions[i3 * 3]) - p1;

            const float s1 = uvs[i2 * 2] - uvs[i1 * 2];
            const float s2 = uvs[i3 * 2] - uvs[i1 * 2];
            const float t1 = uvs[i2 * 2 + 1] - uvs[i1 * 2 + 1];
            const float t2 = uvs[i3 * 2 + 1] - uvs[i1 * 2 + 1];

            const float area = s1 * t2 - s2 * t1;

            Vector3 sdir;
            Vector3 tdir;
            if (area == 0.0f) {
                // Degenerate triangle or UVs: upstream's fallback values.
                sdir = Vector3(0.0f, 1.0f, 0.0f);
                tdir = Vector3(1.0f, 0.0f, 0.0f);
            } else {
                const float r = 1.0f / area;
                sdir = (e1 * t2 - e2 * t1) * r;
                tdir = (e2 * s1 - e1 * s2) * r;
            }

            for (const uint32_t i : {i1, i2, i3}) {
                tan1[i] += sdir;
                tan2[i] += tdir;
            }
        }

        std::vector<float> tangents(vertexCount * 4, 0.0f);
        for (size_t i = 0; i < vertexCount; ++i) {
            const Vector3 n = Vector3::load(&normals[i * 3]);
            const Vector3& t = tan1[i];
            const Vector3& b = tan2[i];

            // Gram-Schmidt orthogonalize.
            Vector3 o = t - n * n.dot(t);
            float len = o.length();
            if (len <= 1e-12f) {
                // DEVIATION: any axis perpendicular to the normal rather than NaN.
                const Vector3 axis = std::fabs(n.getY()) > 0.999f ? Vector3(1.0f, 0.0f, 0.0f) : Vector3(0.0f, 1.0f, 0.0f);
                o = axis - n * n.dot(axis);
                len = o.length();
            }
            (o / len).store(&tangents[i * 4]);

            // Handedness. DEVIATION (see header): the bitangent points toward -v.
            const float towardV = n.cross(t).dot(b);
            tangents[i * 4 + 3] = towardV < 0.0f ? 1.0f : -1.0f;
        }

        return tangents;
    }
}
