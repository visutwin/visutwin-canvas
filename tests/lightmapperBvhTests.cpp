// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The CPU lightmapper's BVH answers one question per ray — is anything hit
// within maxDist — and that answer must not depend on the tree. So the oracle is
// brute force: the same ray/triangle test against every triangle. Any
// disagreement is a box that rejected a ray it should not have, which reads in a
// lightmap as light leaking through an occluder, and nothing in a bake would
// point at the BVH as the cause.
//
// The binary BVH this replaced stored only a node's LEFT child and assumed the
// right one was left + 1. Children are built depth-first, so that holds only when
// the left child is a leaf: every right subtree below an internal left child was
// silently skipped. The architectural scene below is the shape that exposed it.
//
// The slab test is also checked directly against its scalar form, bit for bit, on
// the inputs that break a naive SIMD port: zero direction components (0 * inf =
// NaN, which std::min / std::max ignore and NEON's vminq / vmaxq propagate),
// origins exactly on a box face, and zero-thickness boxes.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "framework/lightmapper/lightmapperBvh.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        if (!condition) {
            std::printf("FAIL: %s\n", what.c_str());
            ++failures;
        }
    }

    bool bruteForce(const std::vector<BvhTriangle>& tris, const Vector3& o, const Vector3& d, const float maxDist)
    {
        for (const BvhTriangle& tri : tris) {
            float t;
            if (rayTriangle(o, d, tri, t) && t < maxDist) return true;
        }
        return false;
    }

    void addQuad(std::vector<BvhTriangle>& tris, const Vector3& p0, const Vector3& p1,
        const Vector3& p2, const Vector3& p3)
    {
        tris.push_back({p0, p1, p2});
        tris.push_back({p0, p2, p3});
    }

    void addBox(std::vector<BvhTriangle>& tris, const Vector3& lo, const Vector3& hi)
    {
        const float x0 = lo.getX(), y0 = lo.getY(), z0 = lo.getZ();
        const float x1 = hi.getX(), y1 = hi.getY(), z1 = hi.getZ();
        addQuad(tris, {x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0});
        addQuad(tris, {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1});
        addQuad(tris, {x0, y0, z0}, {x0, y1, z0}, {x0, y1, z1}, {x0, y0, z1});
        addQuad(tris, {x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1});
        addQuad(tris, {x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1});
        addQuad(tris, {x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1});
    }

    Vector3 randomUnit(std::mt19937& rng)
    {
        std::normal_distribution<float> n(0.0f, 1.0f);
        for (;;) {
            const Vector3 v(n(rng), n(rng), n(rng));
            const float len = v.length();
            if (len > 1e-3f) return v * (1.0f / len);
        }
    }

    void compareScene(const std::string& name, const std::vector<BvhTriangle>& tris,
        std::mt19937& rng, const int rays, const float extent, const bool expectBoth)
    {
        LightmapperBvh bvh;
        bvh.build(tris);

        std::uniform_real_distribution<float> coord(-extent, extent);
        std::uniform_int_distribution<int> pick(0, 9);
        const float maxDists[] = {1e-5f, 0.5f, 5.0f, 50.0f, 1e6f};
        std::uniform_int_distribution<int> pickDist(0, 4);
        const Vector3 axes[] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        std::uniform_int_distribution<int> pickAxis(0, 5);

        int mismatches = 0, hits = 0;
        for (int r = 0; r < rays; ++r) {
            Vector3 origin(coord(rng), coord(rng), coord(rng));
            Vector3 dir = randomUnit(rng);
            const int kind = pick(rng);
            if (kind == 0) {
                dir = axes[pickAxis(rng)];                                   // zero direction components
            } else if (kind == 1) {
                origin = Vector3(std::round(origin.getX()), 0.0f, std::round(origin.getZ()));  // on the floor plane and grid lines
            } else if (kind == 2) {
                origin = Vector3(origin.getX(), 1e-3f, origin.getZ());      // AO ray just above the floor
                if (dir.getY() < 0.0f) dir = Vector3(dir.getX(), -dir.getY(), dir.getZ());
            }
            const float maxDist = maxDists[pickDist(rng)];

            const bool expected = bruteForce(tris, origin, dir, maxDist);
            const bool actual = bvh.anyHit(origin, dir, maxDist);
            if (expected) ++hits;
            if (expected != actual) {
                if (mismatches < 3) {
                    std::printf("  %s mismatch: origin (%g, %g, %g) dir (%g, %g, %g) maxDist %g: brute force %d, BVH %d\n",
                        name.c_str(), origin.getX(), origin.getY(), origin.getZ(),
                        dir.getX(), dir.getY(), dir.getZ(), maxDist, expected, actual);
                }
                ++mismatches;
            }
        }
        std::printf("  %-14s %6zu triangles, %5zu nodes, %5d rays, %5d hits\n", name.c_str(),
            tris.size(), bvh.nodeCount(), rays, hits);
        check(mismatches == 0, name + ": " + std::to_string(mismatches) + " of " + std::to_string(rays) +
            " rays disagree with brute force");
        if (expectBoth) {
            check(hits > rays / 20 && hits < rays - rays / 20,
                name + ": too few hits or misses for the comparison to mean anything (" +
                std::to_string(hits) + " hits)");
        }
    }

    void compareSlab(std::mt19937& rng)
    {
        std::uniform_real_distribution<float> coord(-10.0f, 10.0f);
        std::uniform_int_distribution<int> pick(0, 7);
        const float specials[] = {0.0f, -0.0f, 1.0f, -1.0f, std::numeric_limits<float>::infinity(),
                                  -std::numeric_limits<float>::infinity()};
        int mismatches = 0;
        const int cases = 200000;
        for (int c = 0; c < cases; ++c) {
            float bmin[3][4], bmax[3][4];
            for (int k = 0; k < 4; ++k) {
                for (int a = 0; a < 3; ++a) {
                    float lo = coord(rng), hi = coord(rng);
                    if (lo > hi) std::swap(lo, hi);
                    if (pick(rng) == 0) hi = lo;                        // zero-thickness box
                    bmin[a][k] = lo;
                    bmax[a][k] = hi;
                }
            }
            float origin[3], inv[3];
            for (int a = 0; a < 3; ++a) {
                origin[a] = coord(rng);
                const int kind = pick(rng);
                if (kind == 0) origin[a] = bmin[a][0];                  // exactly on a face
                if (kind == 1) origin[a] = bmax[a][1];
                float dir = coord(rng);
                if (kind == 2 || kind == 3) dir = specials[pick(rng) % 2]; // +-0 direction component
                inv[a] = 1.0f / dir;                                    // +-inf there
                if (kind == 4) inv[a] = specials[2 + pick(rng) % 4];
            }
            const float maxDist = pick(rng) == 0 ? 1e-5f : coord(rng) + 10.0f;
            const LightmapperBvh::Boxes boxes = {bmin[0], bmin[1], bmin[2], bmax[0], bmax[1], bmax[2]};
            const unsigned simd = LightmapperBvh::slabMask(boxes, origin, inv, maxDist);
            const unsigned scalar = LightmapperBvh::slabMaskScalar(boxes, origin, inv, maxDist);
            if (simd != scalar) ++mismatches;
        }
        check(mismatches == 0, "slab test: " + std::to_string(mismatches) + " of " +
            std::to_string(cases) + " cases differ between the SIMD and scalar forms");
    }
}

int main()
{
    std::printf("lightmapper BVH (slab backend: %s)\n", LightmapperBvh::slabBackend());
    if (const char* expected = std::getenv("VISUTWIN_EXPECT_KERNEL_BACKEND");
        expected && *expected && std::string(expected) != LightmapperBvh::slabBackend()) {
        std::printf("FAIL expected the %s backend but this build compiled %s\n",
            expected, LightmapperBvh::slabBackend());
        return 1;
    }

    std::mt19937 rng(20260913);
    compareSlab(rng);

    // An architectural scene: a floor grid and boxes standing on it. Axis-aligned
    // faces give zero-thickness boxes and hits that sit exactly on box faces.
    {
        std::vector<BvhTriangle> tris;
        for (int x = -12; x < 12; ++x) {
            for (int z = -12; z < 12; ++z) {
                addQuad(tris, {float(x), 0, float(z)}, {float(x + 1), 0, float(z)},
                    {float(x + 1), 0, float(z + 1)}, {float(x), 0, float(z + 1)});
            }
        }
        std::uniform_real_distribution<float> place(-10.0f, 9.0f), size(0.5f, 3.0f);
        for (int b = 0; b < 30; ++b) {
            const float x = place(rng), z = place(rng);
            addBox(tris, {x, 0.0f, z}, {x + size(rng), size(rng), z + size(rng)});
        }
        compareScene("architectural", tris, rng, 4000, 12.0f, true);
    }

    // A random triangle soup.
    {
        std::vector<BvhTriangle> tris;
        std::uniform_real_distribution<float> centre(-10.0f, 10.0f), jitter(-1.5f, 1.5f);
        for (int t = 0; t < 1500; ++t) {
            const Vector3 c(centre(rng), centre(rng), centre(rng));
            tris.push_back({c + Vector3(jitter(rng), jitter(rng), jitter(rng)),
                            c + Vector3(jitter(rng), jitter(rng), jitter(rng)),
                            c + Vector3(jitter(rng), jitter(rng), jitter(rng))});
        }
        compareScene("soup", tris, rng, 4000, 12.0f, true);
    }

    // Degenerate: hundreds of identical triangles and one far outlier, which makes
    // the median split fall back to halving and builds a lopsided, deep tree.
    {
        std::vector<BvhTriangle> tris(600, BvhTriangle{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}});
        tris.push_back({{900, 900, 900}, {901, 900, 900}, {900, 901, 900}});
        compareScene("degenerate", tris, rng, 2000, 2.0f, false);
    }

    // One triangle, and none.
    compareScene("single", {BvhTriangle{{-1, 0, -1}, {1, 0, -1}, {0, 0, 1}}}, rng, 2000, 2.0f, false);
    compareScene("empty", {}, rng, 200, 2.0f, false);

    if (failures != 0) {
        std::printf("lightmapper BVH: %d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("lightmapper BVH: all checks passed\n");
    return 0;
}
