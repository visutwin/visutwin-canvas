// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.07.2026.
//
#pragma once

#include <memory>

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Texture;

    /**
     * @brief LTC lookup tables for physically-based area lights.
     * @ingroup group_scene_renderer
     *
     * Creates the two 64x64 RGBA16F textures used by the LTC (linearly transformed
     * cosines) rect-light evaluation: LUT 1 holds the inverse LTC matrix columns
     * (parameterized by sqrt(GGX alpha) x sqrt(1 - cos(view angle))), LUT 2 holds the
     * Fresnel/geometry magnitude terms used for specular energy conservation.
     * DEVIATION: upstream ships the tables as an external JSON asset the app must load
     * (`app.setAreaLightLuts`); this port embeds the half-float data in the engine so
     * area lights work with zero configuration.
     */
    namespace AreaLightLuts
    {
        struct Textures
        {
            std::shared_ptr<Texture> lut1;
            std::shared_ptr<Texture> lut2;
        };

        /// Create both LUT textures (uploaded, linear-filtered, clamp-to-edge).
        Textures create(GraphicsDevice* device);
    }
}
