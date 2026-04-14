// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Shared Gribb/Hartmann frustum plane extraction used by all InstanceCuller
// backends. Moved here from metalInstanceCullPass.cpp so future non-Metal
// backends can reuse the same math.
//
#include "instanceCuller.h"

#include <cmath>

namespace visutwin::canvas
{
    void InstanceCuller::extractFrustumPlanes(
        const float* m, float outPlanes[6][4])
    {
        // Input: 4x4 view-projection matrix in column-major order.
        // m[col*4 + row] — standard Metal/OpenGL layout.
        //
        // Row access helper: row i of column j = m[j*4 + i]
        // Row 0: m[0], m[4], m[8],  m[12]
        // Row 1: m[1], m[5], m[9],  m[13]
        // Row 2: m[2], m[6], m[10], m[14]
        // Row 3: m[3], m[7], m[11], m[15]

        // Left:   row3 + row0
        outPlanes[0][0] = m[3]  + m[0];
        outPlanes[0][1] = m[7]  + m[4];
        outPlanes[0][2] = m[11] + m[8];
        outPlanes[0][3] = m[15] + m[12];

        // Right:  row3 - row0
        outPlanes[1][0] = m[3]  - m[0];
        outPlanes[1][1] = m[7]  - m[4];
        outPlanes[1][2] = m[11] - m[8];
        outPlanes[1][3] = m[15] - m[12];

        // Bottom: row3 + row1
        outPlanes[2][0] = m[3]  + m[1];
        outPlanes[2][1] = m[7]  + m[5];
        outPlanes[2][2] = m[11] + m[9];
        outPlanes[2][3] = m[15] + m[13];

        // Top:    row3 - row1
        outPlanes[3][0] = m[3]  - m[1];
        outPlanes[3][1] = m[7]  - m[5];
        outPlanes[3][2] = m[11] - m[9];
        outPlanes[3][3] = m[15] - m[13];

        // Near:   row3 + row2
        outPlanes[4][0] = m[3]  + m[2];
        outPlanes[4][1] = m[7]  + m[6];
        outPlanes[4][2] = m[11] + m[10];
        outPlanes[4][3] = m[15] + m[14];

        // Far:    row3 - row2
        outPlanes[5][0] = m[3]  - m[2];
        outPlanes[5][1] = m[7]  - m[6];
        outPlanes[5][2] = m[11] - m[10];
        outPlanes[5][3] = m[15] - m[14];

        // Normalize each plane
        for (int i = 0; i < 6; ++i) {
            const float len = std::sqrt(
                outPlanes[i][0] * outPlanes[i][0] +
                outPlanes[i][1] * outPlanes[i][1] +
                outPlanes[i][2] * outPlanes[i][2]);
            if (len > 1e-8f) {
                const float invLen = 1.0f / len;
                outPlanes[i][0] *= invLen;
                outPlanes[i][1] *= invLen;
                outPlanes[i][2] *= invLen;
                outPlanes[i][3] *= invLen;
            }
        }
    }
} // namespace visutwin::canvas
