// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//

#pragma once

namespace visutwin::canvas
{
    /// Scene-level clustered lighting settings (upstream `scene.lighting`).
    struct LightingParams
    {
        bool shadowsEnabled = true;

        bool cookiesEnabled = false;

        /// The cluster grid: space is subdivided into this many cells and each holds
        /// up to `maxLightsPerCell` lights. More cells means fewer lights per cell to
        /// walk in the shader; more lights per cell costs
        /// (cellsX * cellsY * cellsZ * maxLightsPerCell) bytes of index memory.
        int cellsX = 12;
        int cellsY = 16;
        int cellsZ = 12;
        int maxLightsPerCell = 48;

        /// Per-slice resolution of the local shadow atlas, and how many
        /// shadow-casting spot lights it holds. DEVIATION from upstream: this port's
        /// atlas is a texture ARRAY of full-resolution slices rather than one packed
        /// 2D atlas, so there is a capacity rather than a subdivision scheme, and
        /// upstream's atlas split options have no equivalent.
        int shadowAtlasResolution = 1024;
        int shadowAtlasCapacity = 16;
    };
}
