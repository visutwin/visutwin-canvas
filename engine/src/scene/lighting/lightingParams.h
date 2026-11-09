// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025
//

#pragma once

#include <cstdint>
#include <vector>

namespace visutwin::canvas
{
    /// Scene-level clustered lighting settings.
    struct LightingParams
    {
        bool shadowsEnabled = true;

        bool cookiesEnabled = false;
        /// Under clustered lighting a light with
        /// a non-punctual shape shades as an LTC area light only while this is set (off
        /// by default; outside clustered lighting shapes always apply).
        bool areaLightsEnabled = false;
        /// Which area light SHAPES the clustered lighting supports while `areaLightsEnabled`
        /// is set (all by default). A shaped light whose shape is off shades as punctual in
        /// the cluster grid, and the forward variants leave that shape's code out of the
        /// cluster loop, so a scene with rectangular lights alone (the usual case) turns the
        /// other two off. Outside clustered lighting every shape applies.
        bool areaLightRectEnabled = true;
        bool areaLightDiskEnabled = true;
        bool areaLightSphereEnabled = true;

        /// The clustered area light shapes as a mask, 1 << LightShape (bit 0, punctual, never
        /// set); 0 when clustered area lights are off.
        uint32_t areaLightShapes() const
        {
            if (!areaLightsEnabled) {
                return 0u;
            }
            return (areaLightRectEnabled ? 1u << 1 : 0u) | (areaLightDiskEnabled ? 1u << 2 : 0u) |
                (areaLightSphereEnabled ? 1u << 3 : 0u);
        }

        /// The cluster grid: space is subdivided into this many cells and each holds
        /// up to `maxLightsPerCell` lights. More cells means fewer lights per cell to
        /// walk in the shader; more lights per cell costs
        /// (cellsX * cellsY * cellsZ * maxLightsPerCell) bytes of index memory.
        int cellsX = 12;
        int cellsY = 16;
        int cellsZ = 12;
        int maxLightsPerCell = 48;

        /// Resolution of the packed local shadow atlas every clustered spot and omni
        /// shadow renders into (2048 by default: 16 MB of 32-bit depth, whatever
        /// the light count). Applied when the atlas is first created.
        int shadowAtlasResolution = 2048;
        /// How the atlas is split into slots. Empty (the default) splits it into as
        /// many equal squares as there are shadow-casting lights; otherwise
        /// `atlasSplit[0]` squares on a side, and `atlasSplit[1 + i * n + j]` may
        /// split cell (i, j) again, for scenes that want a few
        /// large slots and many small ones.
        std::vector<int> atlasSplit;
        /// Resolution of the clustered cookie atlas (2048 by default), laid out
        /// like the shadow atlas; created only once a clustered light has a cookie and
        /// `cookiesEnabled` is set.
        int cookieAtlasResolution = 2048;
    };
}
