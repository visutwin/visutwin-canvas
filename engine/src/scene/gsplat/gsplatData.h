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
     * Two PLY layouts are auto-detected:
     *  - **Uncompressed** (`binary_little_endian` float `vertex` element): x/y/z,
     *    f_dc_0..2, opacity, scale_0..2, rot_0..3, and optional f_rest_* SH bands
     *    (9/24/45 coefficients → SH bands 1/2/3).
     *  - **Compressed** (SuperSplat `.compressed.ply`): a `chunk` element (per-256
     *    min/max bounds) + a uint `vertex` element (packed 11-10-11 position/scale,
     *    2-10-10-10 rotation, 8888 color) + an optional uchar `sh` element.
     *
     * Higher-order SH coefficients (bands 1-3) are dequantized into `shCoeffs()`
     * laid out coefficient-major interleaved: `[c0.r,c0.g,c0.b, c1.r,...]`, 15
     * coefficients × 3 channels = 45 floats per splat, zero-padded above the
     * present band. The gsplat vertex shader evaluates them per frame by view
     * direction. DEVIATION: upstream quantizes SH to 11-10-11 in a texture; this
     * port stores raw floats in a storage buffer.
     */
    class GSplatData
    {
    public:
        static std::unique_ptr<GSplatData> loadPly(const std::string& path);

        int numSplats() const { return static_cast<int>(_splats.size()); }
        const std::vector<GpuSplat>& splats() const { return _splats; }
        const std::vector<float>& centers() const { return _centers; }  // xyz per splat
        const BoundingBox& aabb() const { return _aabb; }

        int shBands() const { return _shBands; }   // 0 = SH0 only (no view-dependent color)
        // 45 floats per splat (coefficient-major interleaved), empty when shBands == 0.
        const std::vector<float>& shCoeffs() const { return _shCoeffs; }

    private:
        std::vector<GpuSplat> _splats;
        std::vector<float> _centers;
        std::vector<float> _shCoeffs;
        int _shBands = 0;
        BoundingBox _aabb;
    };
}
