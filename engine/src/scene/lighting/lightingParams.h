// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//

#pragma once

#include <vector>

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

        /// Resolution of the packed local shadow atlas every clustered spot and omni
        /// shadow renders into (upstream default 2048: 16 MB of 32-bit depth, whatever
        /// the light count). Applied when the atlas is first created.
        int shadowAtlasResolution = 2048;
        /// How the atlas is split into slots. Empty (the default) splits it into as
        /// many equal squares as there are shadow-casting lights; otherwise
        /// `atlasSplit[0]` squares on a side, and `atlasSplit[1 + i * n + j]` may
        /// split cell (i, j) again — upstream's scheme, for scenes that want a few
        /// large slots and many small ones.
        std::vector<int> atlasSplit;
    };
}
