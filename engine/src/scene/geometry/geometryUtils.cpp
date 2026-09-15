// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream geometry-utils calculateTangents. See the header for the two
// deviations.
//
#include "geometryUtils.h"

#include <cmath>
#include <cstddef>

namespace visutwin::canvas
{
    std::vector<float> calculateTangents(const std::vector<float>& positions,
        const std::vector<float>& normals, const std::vector<float>& uvs,
        const std::vector<uint32_t>& indices)
    {
        const size_t vertexCount = positions.size() / 3;
        std::vector<float> tan1(vertexCount * 3, 0.0f);
        std::vector<float> tan2(vertexCount * 3, 0.0f);

        for (size_t tri = 0; tri + 2 < indices.size(); tri += 3) {
            const uint32_t i1 = indices[tri];
            const uint32_t i2 = indices[tri + 1];
            const uint32_t i3 = indices[tri + 2];
            if (i1 >= vertexCount || i2 >= vertexCount || i3 >= vertexCount) {
                continue;
            }

            const float x1 = positions[i2 * 3] - positions[i1 * 3];
            const float x2 = positions[i3 * 3] - positions[i1 * 3];
            const float y1 = positions[i2 * 3 + 1] - positions[i1 * 3 + 1];
            const float y2 = positions[i3 * 3 + 1] - positions[i1 * 3 + 1];
            const float z1 = positions[i2 * 3 + 2] - positions[i1 * 3 + 2];
            const float z2 = positions[i3 * 3 + 2] - positions[i1 * 3 + 2];

            const float s1 = uvs[i2 * 2] - uvs[i1 * 2];
            const float s2 = uvs[i3 * 2] - uvs[i1 * 2];
            const float t1 = uvs[i2 * 2 + 1] - uvs[i1 * 2 + 1];
            const float t2 = uvs[i3 * 2 + 1] - uvs[i1 * 2 + 1];

            const float area = s1 * t2 - s2 * t1;

            float sdir[3];
            float tdir[3];
            if (area == 0.0f) {
                // Degenerate triangle or UVs: upstream's fallback values.
                sdir[0] = 0.0f; sdir[1] = 1.0f; sdir[2] = 0.0f;
                tdir[0] = 1.0f; tdir[1] = 0.0f; tdir[2] = 0.0f;
            } else {
                const float r = 1.0f / area;
                sdir[0] = (t2 * x1 - t1 * x2) * r;
                sdir[1] = (t2 * y1 - t1 * y2) * r;
                sdir[2] = (t2 * z1 - t1 * z2) * r;
                tdir[0] = (s1 * x2 - s2 * x1) * r;
                tdir[1] = (s1 * y2 - s2 * y1) * r;
                tdir[2] = (s1 * z2 - s2 * z1) * r;
            }

            for (const uint32_t i : {i1, i2, i3}) {
                for (int k = 0; k < 3; ++k) {
                    tan1[i * 3 + k] += sdir[k];
                    tan2[i * 3 + k] += tdir[k];
                }
            }
        }

        std::vector<float> tangents(vertexCount * 4, 0.0f);
        for (size_t i = 0; i < vertexCount; ++i) {
            const float n[3] = {normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2]};
            const float* t = &tan1[i * 3];
            const float* b = &tan2[i * 3];

            // Gram-Schmidt orthogonalize.
            const float ndott = n[0] * t[0] + n[1] * t[1] + n[2] * t[2];
            float o[3] = {t[0] - n[0] * ndott, t[1] - n[1] * ndott, t[2] - n[2] * ndott};
            float len = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
            if (len <= 1e-12f) {
                // DEVIATION: any axis perpendicular to the normal rather than NaN.
                const bool nearY = std::fabs(n[1]) > 0.999f;
                const float axis[3] = {nearY ? 1.0f : 0.0f, nearY ? 0.0f : 1.0f, 0.0f};
                const float d = n[0] * axis[0] + n[1] * axis[1] + n[2] * axis[2];
                o[0] = axis[0] - n[0] * d;
                o[1] = axis[1] - n[1] * d;
                o[2] = axis[2] - n[2] * d;
                len = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
            }
            tangents[i * 4] = o[0] / len;
            tangents[i * 4 + 1] = o[1] / len;
            tangents[i * 4 + 2] = o[2] / len;

            // Handedness. DEVIATION (see header): the bitangent points toward -v.
            const float c[3] = {
                n[1] * t[2] - n[2] * t[1],
                n[2] * t[0] - n[0] * t[2],
                n[0] * t[1] - n[1] * t[0]};
            const float towardV = c[0] * b[0] + c[1] * b[1] + c[2] * b[2];
            tangents[i * 4 + 3] = towardV < 0.0f ? 1.0f : -1.0f;
        }

        return tangents;
    }
}
