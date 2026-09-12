// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// A splat is an ellipsoid, so the cloud reaches past the hull of its centres and a
// bound built from centres alone culls it while part of it is still on screen.
//
// What the bound must be is exact rather than approximate, because this parser keeps
// the composed covariance Sigma = R S^2 R^T. Its diagonal is the variance along each
// model axis, so 2 * sqrt(Sigma_dd) is the tightest axis-aligned bound on the
// 2-sigma ellipsoid — the same convention upstream uses, tighter than either of the
// two bounds upstream computes. These cases pin that: a rotated splat's bound has to
// follow the ROTATED extent, which is the half a naive "pad x by scale_0" gets wrong
// and which no amount of looking at a render would reveal.
//
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/math/vector3.h"
#include "scene/gsplat/gsplatData.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const char* what)
    {
        if (!condition) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
    }

    void checkClose(const float actual, const float expected, const char* what,
        const float tolerance = 1e-4f)
    {
        if (std::fabs(actual - expected) > tolerance) {
            std::printf("FAIL: %s — expected %.6f, got %.6f\n", what, expected, actual);
            ++failures;
        }
    }

    /// One splat as the uncompressed PLY layout carries it: scales are LOG-space and
    /// the quaternion is (w, x, y, z), which is what the parser exponentiates and
    /// normalizes on the way to a covariance.
    struct PlySplat
    {
        float x, y, z;
        float scale[3];      // world-space; written as log
        float quat[4];       // w, x, y, z
    };

    std::string writePly(const std::vector<PlySplat>& splats, const char* name)
    {
        const auto path = (std::filesystem::temp_directory_path() / name).string();
        std::ofstream file(path, std::ios::binary);

        file << "ply\n"
             << "format binary_little_endian 1.0\n"
             << "element vertex " << splats.size() << "\n"
             << "property float x\nproperty float y\nproperty float z\n"
             << "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
             << "property float opacity\n"
             << "property float scale_0\nproperty float scale_1\nproperty float scale_2\n"
             << "property float rot_0\nproperty float rot_1\n"
             << "property float rot_2\nproperty float rot_3\n"
             << "end_header\n";

        for (const auto& s : splats) {
            const float row[14] = {
                s.x, s.y, s.z,
                0.5f, 0.5f, 0.5f,            // f_dc — irrelevant to the bound
                3.0f,                        // opacity — likewise
                std::log(s.scale[0]), std::log(s.scale[1]), std::log(s.scale[2]),
                s.quat[0], s.quat[1], s.quat[2], s.quat[3]
            };
            file.write(reinterpret_cast<const char*>(row), sizeof(row));
        }
        return path;
    }

    /// Loads a temporary PLY and hands back its bounds, deleting the file either way.
    bool boundsOf(const std::vector<PlySplat>& splats, const char* name,
        Vector3& outMin, Vector3& outMax)
    {
        const auto path = writePly(splats, name);
        const auto data = GSplatData::loadPly(path);
        std::filesystem::remove(path);
        if (!data) {
            return false;
        }
        const auto& aabb = data->aabb();
        outMin = aabb.center() - aabb.halfExtents();
        outMax = aabb.center() + aabb.halfExtents();
        return true;
    }
}

int main()
{
    const float identity[4] = {1.0f, 0.0f, 0.0f, 0.0f};

    // ── An axis-aligned splat: the bound is 2 sigma on each axis ──────────────
    {
        PlySplat splat{0.0f, 0.0f, 0.0f, {0.5f, 0.25f, 0.125f}, {}};
        std::memcpy(splat.quat, identity, sizeof(identity));

        Vector3 boundsMin, boundsMax;
        if (!boundsOf({splat}, "vt-gsplat-aabb-axis.ply", boundsMin, boundsMax)) {
            std::printf("FAIL: axis-aligned case did not load\n");
            return 1;
        }

        // Identity rotation leaves Sigma = diag(s^2), so each axis gets 2 * s.
        checkClose(boundsMax.getX(), 1.0f, "axis-aligned +x");
        checkClose(boundsMax.getY(), 0.5f, "axis-aligned +y");
        checkClose(boundsMax.getZ(), 0.25f, "axis-aligned +z");
        checkClose(boundsMin.getX(), -1.0f, "axis-aligned -x");
        checkClose(boundsMin.getY(), -0.5f, "axis-aligned -y");
        checkClose(boundsMin.getZ(), -0.25f, "axis-aligned -z");
    }

    // ── The same splat turned 90 degrees about Z: the extents SWAP ────────────
    // The case that separates an honest bound from one that pads axis d by scale_d.
    {
        const float halfRoot2 = std::sqrt(0.5f);
        PlySplat splat{0.0f, 0.0f, 0.0f, {0.5f, 0.25f, 0.125f},
            {halfRoot2, 0.0f, 0.0f, halfRoot2}};   // w, x, y, z

        Vector3 boundsMin, boundsMax;
        if (!boundsOf({splat}, "vt-gsplat-aabb-rot.ply", boundsMin, boundsMax)) {
            std::printf("FAIL: rotated case did not load\n");
            return 1;
        }

        checkClose(boundsMax.getX(), 0.5f, "rotated +x takes the y scale");
        checkClose(boundsMax.getY(), 1.0f, "rotated +y takes the x scale");
        checkClose(boundsMax.getZ(), 0.25f, "rotated +z is unchanged");
    }

    // ── A 45-degree rotation: exactly the ellipsoid's own bound ───────────────
    // Sigma_xx = Sigma_yy = (sx^2 + sy^2) / 2 for a half-turn-of-a-quarter about Z,
    // which is strictly between the two scales — so a bound that merely picked one
    // of them, or padded by the largest as upstream's pessimistic path does, lands
    // somewhere else. This is where the DEVIATION is worth the arithmetic.
    {
        const float cos22 = std::cos(0.39269908f);   // 22.5 degrees = half of 45
        const float sin22 = std::sin(0.39269908f);
        const float sx = 0.8f, sy = 0.2f, sz = 0.1f;
        PlySplat splat{0.0f, 0.0f, 0.0f, {sx, sy, sz}, {cos22, 0.0f, 0.0f, sin22}};

        Vector3 boundsMin, boundsMax;
        if (!boundsOf({splat}, "vt-gsplat-aabb-45.ply", boundsMin, boundsMax)) {
            std::printf("FAIL: 45-degree case did not load\n");
            return 1;
        }

        const float expected = 2.0f * std::sqrt(0.5f * (sx * sx + sy * sy));
        checkClose(boundsMax.getX(), expected, "45-degree +x is the exact ellipsoid bound");
        checkClose(boundsMax.getY(), expected, "45-degree +y is the exact ellipsoid bound");

        // And it is genuinely tighter than upstream's isotropic 2 * max(scale) pad,
        // which is the whole reason for keeping the covariance rather than the scales.
        check(boundsMax.getX() < 2.0f * sx,
            "45-degree bound is tighter than an isotropic largest-scale pad");
        checkClose(boundsMax.getZ(), 2.0f * sz, "45-degree +z keeps its own scale");
    }

    // ── Two splats: the union, and strictly bigger than the centre hull ───────
    {
        const std::vector<PlySplat> splats = {
            {-3.0f, 0.0f, 0.0f, {0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f, 0.0f}},
            { 3.0f, 0.0f, 0.0f, {0.25f, 0.25f, 0.25f}, {1.0f, 0.0f, 0.0f, 0.0f}}
        };

        Vector3 boundsMin, boundsMax;
        if (!boundsOf(splats, "vt-gsplat-aabb-pair.ply", boundsMin, boundsMax)) {
            std::printf("FAIL: two-splat case did not load\n");
            return 1;
        }

        checkClose(boundsMin.getX(), -4.0f, "union takes the left splat's own extent");
        checkClose(boundsMax.getX(), 3.5f, "union takes the right splat's own extent");

        // The regression this file exists for: a centres-only bound would stop at
        // +/-3 and cull the cloud while its rim was still on screen.
        check(boundsMin.getX() < -3.0f && boundsMax.getX() > 3.0f,
            "bounds extend past the hull of the centres");
    }

    if (failures == 0) {
        std::printf("gsplat aabb: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
