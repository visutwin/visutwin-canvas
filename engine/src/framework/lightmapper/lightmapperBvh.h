// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/math/vector3.h"

namespace visutwin::canvas
{
    struct BvhTriangle
    {
        Vector3 a, b, c;
    };

    /// Möller-Trumbore ray/triangle. Returns the hit distance in `t`; hits closer
    /// than 1e-4 are rejected as self-intersection.
    bool rayTriangle(const Vector3& origin, const Vector3& direction, const BvhTriangle& tri, float& t);

    /**
     * Any-hit acceleration structure for the CPU lightmapper's shadow and AO rays.
     * Without one, a per-texel bake is O(texels · rays · triangles) — minutes, not
     * seconds.
     *
     * A median-split binary BVH, collapsed into FOUR children per node so a single
     * SIMD slab test (SSE2 on x86, NEON on AArch64, scalar otherwise) covers every
     * child at once. Leaves hold at most four triangles.
     *
     * Two properties the test relies on and a change here must keep:
     *   - An any-hit answer does not depend on the tree's shape or the order it is
     *     walked in, provided a box never rejects a ray that hits a triangle inside
     *     it. The boxes are PADDED so float rounding in the slab test cannot, which
     *     makes brute force over every triangle an exact oracle.
     *   - `slabMask` reproduces the scalar `slabMaskScalar` bit for bit, NaN
     *     included: a zero direction component makes 0 * inf = NaN, and the scalar
     *     test's std::min / std::max IGNORE a NaN, which a plain SIMD min / max
     *     (NaN-propagating on NEON) would not.
     */
    class LightmapperBvh
    {
    public:
        void build(const std::vector<BvhTriangle>& triangles);

        /// True if any triangle is hit at a distance in [1e-4, maxDist).
        bool anyHit(const Vector3& origin, const Vector3& direction, float maxDist) const;

        [[nodiscard]] size_t nodeCount() const { return _nodes.size(); }

        /// Four boxes as six 4-float arrays: min x, y, z then max x, y, z. Returns
        /// bit k set when box k may be hit within [0, maxDist]. Public for its test.
        using Boxes = std::array<const float*, 6>;
        static unsigned slabMask(const Boxes& boxes, const float origin[3],
            const float inverseDirection[3], float maxDist);
        static unsigned slabMaskScalar(const Boxes& boxes, const float origin[3],
            const float inverseDirection[3], float maxDist);

        /// Which implementation `slabMask` compiled to: "sse2", "neon" or "scalar".
        static const char* slabBackend();

    private:
        struct Builder;

        struct Node
        {
            std::array<float, 4> bminX{}, bminY{}, bminZ{};
            std::array<float, 4> bmaxX{}, bmaxY{}, bmaxZ{};
            /// Internal child: node index. Leaf child: first triangle in `_triangles`.
            std::array<int32_t, 4> child{};
            /// Triangles in a leaf child; 0 for an internal child.
            std::array<uint8_t, 4> count{};
            /// Bit k set when child k exists.
            uint8_t valid = 0;
        };

        std::vector<Node> _nodes;
        std::vector<BvhTriangle> _triangles;  // in leaf order, so each leaf is contiguous
        int _maxDepth = 0;
    };
}
