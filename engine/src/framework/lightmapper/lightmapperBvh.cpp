// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Define VISUTWIN_KERNELS_FORCE_SCALAR to build the scalar slab test on a SIMD
// target, for measuring what the SIMD path is worth.
//
#include "lightmapperBvh.h"

#include <algorithm>
#include <bit>
#include <cmath>

#if !defined(VISUTWIN_KERNELS_FORCE_SCALAR) && defined(__SSE2__)
    #include <emmintrin.h>
    #include <xmmintrin.h>
#elif !defined(VISUTWIN_KERNELS_FORCE_SCALAR) && defined(__ARM_NEON) && defined(__aarch64__)
    #include <arm_neon.h>
#endif

namespace visutwin::canvas
{
    bool rayTriangle(const Vector3& o, const Vector3& d, const BvhTriangle& tri, float& t)
    {
        const Vector3 e1 = tri.b - tri.a, e2 = tri.c - tri.a;
        const Vector3 pv = d.cross(e2);
        const float det = e1.dot(pv);
        if (std::fabs(det) < 1e-8f) return false;
        const float inv = 1.0f / det;
        const Vector3 tv = o - tri.a;
        const float u = tv.dot(pv) * inv;
        if (u < 0.0f || u > 1.0f) return false;
        const Vector3 qv = tv.cross(e1);
        const float v = d.dot(qv) * inv;
        if (v < 0.0f || u + v > 1.0f) return false;
        t = e2.dot(qv) * inv;
        return t > 1e-4f;
    }

    namespace
    {
        constexpr int LEAF_SIZE = 4;

        struct BinaryNode
        {
            Vector3 bmin, bmax;
            int start = 0, count = 0;
            // BOTH children are stored. Children are built depth-first, so the
            // right child's index is NOT left + 1 unless the left child is a leaf.
            int left = -1, right = -1;
        };

        float surfaceArea(const BinaryNode& n)
        {
            const Vector3 e = n.bmax - n.bmin;
            return e.getX() * e.getY() + e.getY() * e.getZ() + e.getZ() * e.getX();
        }
    }

    struct LightmapperBvh::Builder
    {
        const std::vector<BvhTriangle>& triangles;
        std::vector<int> order;
        std::vector<BinaryNode> binary;

        int buildBinary(const int start, const int count)
        {
            const int index = static_cast<int>(binary.size());
            binary.emplace_back();

            Vector3 bmin(1e30f), bmax(-1e30f);
            Vector3 cmin(1e30f), cmax(-1e30f);
            for (int i = 0; i < count; ++i) {
                const BvhTriangle& t = triangles[static_cast<size_t>(order[static_cast<size_t>(start + i)])];
                // min(min(a, b), c) is std::min({a, b, c}) lane by lane, NaN handling included.
                const Vector3 lo = Vector3::min(Vector3::min(t.a, t.b), t.c);
                const Vector3 hi = Vector3::max(Vector3::max(t.a, t.b), t.c);
                bmin = Vector3::min(bmin, lo);
                bmax = Vector3::max(bmax, hi);
                const Vector3 c = (lo + hi) * 0.5f;
                cmin = Vector3::min(cmin, c);
                cmax = Vector3::max(cmax, c);
            }

            if (count <= LEAF_SIZE) {
                binary[static_cast<size_t>(index)] = {bmin, bmax, start, count, -1, -1};
                return index;
            }

            const Vector3 ext = cmax - cmin;
            const float extX = ext.getX(), extY = ext.getY(), extZ = ext.getZ();
            const int axis = extX > extY ? (extX > extZ ? 0 : 2) : (extY > extZ ? 1 : 2);
            const float mid = 0.5f * (cmin[axis] + cmax[axis]);
            const int* first = order.data() + start;
            const int* split = std::partition(order.data() + start, order.data() + start + count,
                [&](const int idx) {
                    const BvhTriangle& t = triangles[static_cast<size_t>(idx)];
                    return ((t.a + t.b + t.c) * (1.0f / 3.0f))[axis] < mid;
                });
            int leftCount = static_cast<int>(split - first);
            if (leftCount == 0 || leftCount == count) leftCount = count / 2;  // degenerate guard

            const int left = buildBinary(start, leftCount);
            const int right = buildBinary(start + leftCount, count - leftCount);
            binary[static_cast<size_t>(index)] = {bmin, bmax, start, count, left, right};
            return index;
        }
    };

    void LightmapperBvh::build(const std::vector<BvhTriangle>& triangles)
    {
        _nodes.clear();
        _triangles.clear();
        _maxDepth = 0;
        if (triangles.empty()) {
            return;
        }

        Builder builder{triangles, {}, {}};
        builder.order.resize(triangles.size());
        for (size_t i = 0; i < triangles.size(); ++i) builder.order[i] = static_cast<int>(i);
        builder.binary.reserve(triangles.size() * 2);
        builder.buildBinary(0, static_cast<int>(triangles.size()));

        // Leaves index into a copy of the triangles in leaf order, so a leaf's
        // triangles sit next to each other in memory.
        _triangles.reserve(triangles.size());
        for (const int idx : builder.order) _triangles.push_back(triangles[static_cast<size_t>(idx)]);

        // Pad every box by an amount tied to the scene's coordinate scale. The slab
        // test works in floats, so a triangle lying exactly on a box face — every
        // floor and wall in an architectural scene — can have its hit rounded just
        // outside the box. A box that rejected such a ray would lose a real hit;
        // padding only ever adds candidates, which the triangle test then decides.
        const BinaryNode& root = builder.binary.front();
        const float scale = std::max(1.0f, Vector3::max(root.bmin.abs(), root.bmax.abs()).maxComponent());
        const float pad = scale * 1e-5f;

        _nodes.reserve(builder.binary.size() / 2 + 1);

        // Collapse: each 4-wide node takes the binary node's children and keeps
        // opening the internal child with the largest surface area until it has four.
        struct Collapse
        {
            LightmapperBvh& bvh;
            const std::vector<BinaryNode>& binary;
            float pad;

            int operator()(const int binaryIndex, const int depth)
            {
                const int index = static_cast<int>(bvh._nodes.size());
                bvh._nodes.emplace_back();
                bvh._maxDepth = std::max(bvh._maxDepth, depth);

                std::array<int, 4> slots{};
                int numSlots = 0;
                const BinaryNode& self = binary[static_cast<size_t>(binaryIndex)];
                if (self.left < 0) {
                    slots[numSlots++] = binaryIndex;
                } else {
                    slots[numSlots++] = self.left;
                    slots[numSlots++] = self.right;
                }
                while (numSlots < 4) {
                    int best = -1;
                    float bestArea = -1.0f;
                    for (int k = 0; k < numSlots; ++k) {
                        const BinaryNode& n = binary[static_cast<size_t>(slots[static_cast<size_t>(k)])];
                        if (n.left >= 0 && surfaceArea(n) > bestArea) {
                            bestArea = surfaceArea(n);
                            best = k;
                        }
                    }
                    if (best < 0) break;
                    const BinaryNode& opened = binary[static_cast<size_t>(slots[static_cast<size_t>(best)])];
                    slots[static_cast<size_t>(best)] = opened.left;
                    slots[static_cast<size_t>(numSlots++)] = opened.right;
                }

                Node node;
                for (int k = 0; k < numSlots; ++k) {
                    const BinaryNode& n = binary[static_cast<size_t>(slots[static_cast<size_t>(k)])];
                    node.bminX[k] = n.bmin.getX() - pad;
                    node.bminY[k] = n.bmin.getY() - pad;
                    node.bminZ[k] = n.bmin.getZ() - pad;
                    node.bmaxX[k] = n.bmax.getX() + pad;
                    node.bmaxY[k] = n.bmax.getY() + pad;
                    node.bmaxZ[k] = n.bmax.getZ() + pad;
                    if (n.left < 0) {
                        node.child[k] = n.start;
                        node.count[k] = static_cast<uint8_t>(n.count);
                    }
                    node.valid = static_cast<uint8_t>(node.valid | (1u << k));
                }
                bvh._nodes[static_cast<size_t>(index)] = node;

                for (int k = 0; k < numSlots; ++k) {
                    const BinaryNode& n = binary[static_cast<size_t>(slots[static_cast<size_t>(k)])];
                    if (n.left >= 0) {
                        const int child = (*this)(slots[static_cast<size_t>(k)], depth + 1);
                        bvh._nodes[static_cast<size_t>(index)].child[k] = child;
                    }
                }
                return index;
            }
        };
        Collapse{*this, builder.binary, pad}(0, 0);
    }

    bool LightmapperBvh::anyHit(const Vector3& origin, const Vector3& direction, const float maxDist) const
    {
        if (_nodes.empty()) return false;

        const float o[3] = {origin.getX(), origin.getY(), origin.getZ()};
        const float inv[3] = {1.0f / direction.getX(), 1.0f / direction.getY(), 1.0f / direction.getZ()};

        // Each level pops one node and pushes at most four, so the stack never holds
        // more than three per level plus one. anyHit runs on many bake threads at
        // once, so the stack is local; a tree too deep for the fixed one gets a heap one.
        const size_t stackNeeded = static_cast<size_t>(_maxDepth) * 3u + 4u;
        std::array<int32_t, 256> fixedStack;
        std::vector<int32_t> heapStack;
        int32_t* stack = fixedStack.data();
        if (stackNeeded > fixedStack.size()) {
            heapStack.resize(stackNeeded);
            stack = heapStack.data();
        }

        size_t sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const Node& node = _nodes[static_cast<size_t>(stack[--sp])];
            const Boxes boxes = {node.bminX.data(), node.bminY.data(), node.bminZ.data(),
                                 node.bmaxX.data(), node.bmaxY.data(), node.bmaxZ.data()};
            unsigned hits = slabMask(boxes, o, inv, maxDist) & node.valid;
            while (hits != 0u) {
                const int k = std::countr_zero(hits);
                hits &= hits - 1u;
                if (node.count[k] > 0) {
                    const int first = node.child[k];
                    const int last = first + node.count[k];
                    for (int i = first; i < last; ++i) {
                        float t;
                        if (rayTriangle(origin, direction, _triangles[static_cast<size_t>(i)], t) && t < maxDist) {
                            return true;
                        }
                    }
                } else {
                    stack[sp++] = node.child[k];
                }
            }
        }
        return false;
    }

    unsigned LightmapperBvh::slabMaskScalar(const Boxes& b, const float origin[3],
        const float inv[3], const float maxDist)
    {
        unsigned mask = 0u;
        for (int k = 0; k < 4; ++k) {
            float t0 = 0.0f, t1 = maxDist;
            bool inside = true;
            for (int a = 0; a < 3; ++a) {
                float nearT = (b[static_cast<size_t>(a)][k] - origin[a]) * inv[a];
                float farT = (b[static_cast<size_t>(a + 3)][k] - origin[a]) * inv[a];
                if (nearT > farT) std::swap(nearT, farT);
                t0 = std::max(t0, nearT);
                t1 = std::min(t1, farT);
                if (t0 > t1) { inside = false; break; }
            }
            if (inside) mask |= 1u << k;
        }
        return mask;
    }

#if !defined(VISUTWIN_KERNELS_FORCE_SCALAR) && defined(__SSE2__)
    const char* LightmapperBvh::slabBackend() { return "sse2"; }

    unsigned LightmapperBvh::slabMask(const Boxes& b, const float origin[3],
        const float inv[3], const float maxDist)
    {
        // select(m, a, b) = m ? a : b, bit-exact.
        const auto select = [](const __m128 m, const __m128 a, const __m128 bb) {
            return _mm_or_ps(_mm_and_ps(m, a), _mm_andnot_ps(m, bb));
        };
        __m128 t0 = _mm_setzero_ps();
        __m128 t1 = _mm_set1_ps(maxDist);
        for (int a = 0; a < 3; ++a) {
            const __m128 o = _mm_set1_ps(origin[a]);
            const __m128 i = _mm_set1_ps(inv[a]);
            const __m128 lo = _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(b[static_cast<size_t>(a)]), o), i);
            const __m128 hi = _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(b[static_cast<size_t>(a + 3)]), o), i);
            const __m128 swap = _mm_cmpgt_ps(lo, hi);          // false for NaN, as in the scalar test
            const __m128 nearT = select(swap, hi, lo);
            const __m128 farT = select(swap, lo, hi);
            t0 = select(_mm_cmplt_ps(t0, nearT), nearT, t0);   // std::max(t0, nearT): keeps t0 on NaN
            t1 = select(_mm_cmplt_ps(farT, t1), farT, t1);     // std::min(t1, farT): keeps t1 on NaN
        }
        // Rejected once, rejected for good: t0 only grows and t1 only shrinks, so
        // testing at the end matches the scalar test's early exit.
        const int rejected = _mm_movemask_ps(_mm_cmpgt_ps(t0, t1));
        return static_cast<unsigned>(~rejected) & 0xFu;
    }
#elif !defined(VISUTWIN_KERNELS_FORCE_SCALAR) && defined(__ARM_NEON) && defined(__aarch64__)
    const char* LightmapperBvh::slabBackend() { return "neon"; }

    unsigned LightmapperBvh::slabMask(const Boxes& b, const float origin[3],
        const float inv[3], const float maxDist)
    {
        float32x4_t t0 = vdupq_n_f32(0.0f);
        float32x4_t t1 = vdupq_n_f32(maxDist);
        for (int a = 0; a < 3; ++a) {
            const float32x4_t o = vdupq_n_f32(origin[a]);
            const float32x4_t i = vdupq_n_f32(inv[a]);
            const float32x4_t lo = vmulq_f32(vsubq_f32(vld1q_f32(b[static_cast<size_t>(a)]), o), i);
            const float32x4_t hi = vmulq_f32(vsubq_f32(vld1q_f32(b[static_cast<size_t>(a + 3)]), o), i);
            const uint32x4_t swap = vcgtq_f32(lo, hi);          // false for NaN
            const float32x4_t nearT = vbslq_f32(swap, hi, lo);
            const float32x4_t farT = vbslq_f32(swap, lo, hi);
            // NOT vmaxq_f32 / vminq_f32: those propagate NaN, where std::max / std::min keep t0 / t1.
            t0 = vbslq_f32(vcltq_f32(t0, nearT), nearT, t0);
            t1 = vbslq_f32(vcltq_f32(farT, t1), farT, t1);
        }
        static const uint32_t laneBits[4] = {1u, 2u, 4u, 8u};
        const uint32_t rejected = vaddvq_u32(vandq_u32(vcgtq_f32(t0, t1), vld1q_u32(laneBits)));
        return static_cast<unsigned>(~rejected) & 0xFu;
    }
#else
    const char* LightmapperBvh::slabBackend() { return "scalar"; }

    unsigned LightmapperBvh::slabMask(const Boxes& b, const float origin[3],
        const float inv[3], const float maxDist)
    {
        return slabMaskScalar(b, origin, inv, maxDist);
    }
#endif
}
