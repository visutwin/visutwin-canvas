// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <core/shape/boundingBox.h>

namespace visutwin::canvas
{
    /**
     * Per-splat GPU record (40 bytes), bound as a storage buffer at vertex slot 7.
     * DEVIATION: upstream packs splat data into WebGL textures and computes the 3D
     * covariance in the vertex shader; this port precomputes covariance on the CPU
     * (Sigma = R * S^2 * R^T, upper triangular as covA/covB) and uses one Metal buffer.
     */
    struct GpuSplat
    {
        float center[3];
        uint32_t color;    // RGBA8: SH0-decoded color + sigmoid opacity
        float covA[3];     // Sigma00, Sigma01, Sigma02
        float covB[3];     // Sigma11, Sigma12, Sigma22
    };
    static_assert(sizeof(GpuSplat) == 40);

    /**
     * CPU-side Gaussian splat set parsed from a 3DGS PLY file: packed GPU records,
     * raw centers for the depth sorter, and the model-space bounds.
     *
     * Supports the standard 3D Gaussian Splatting binary_little_endian PLY layout
     * (x/y/z, f_dc_0..2, opacity, scale_0..2, rot_0..3; higher-order SH bands are
     * skipped — DEVIATION: view-dependent SH color is not evaluated, SH0 only).
     */
    class GSplatData
    {
    public:
        static std::unique_ptr<GSplatData> loadPly(const std::string& path);

        int numSplats() const { return static_cast<int>(_splats.size()); }
        const std::vector<GpuSplat>& splats() const { return _splats; }
        const std::vector<float>& centers() const { return _centers; }  // xyz per splat
        const BoundingBox& aabb() const { return _aabb; }

    private:
        std::vector<GpuSplat> _splats;
        std::vector<float> _centers;
        BoundingBox _aabb;
    };
}
