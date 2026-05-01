// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2025.
//
#pragma once

#include <cstdint>

namespace visutwin::canvas
{
    /**
     * DepthState is a descriptor that defines how the depth value of the fragment is used by the
     * rendering pipeline. A depth state can be set on a material using {@link Material#depthState},
     * or in some cases on the graphics device using {@link GraphicsDevice#setDepthState}.
     *
     * For the best performance, do not modify the depth state after it has been created, but create
     * multiple depth states and assign them to the material or graphics device as needed.
     */
    class DepthState
    {
    public:
        /**
         * A unique number representing the depth state. You can use this number to quickly compare
         * two depth states for equality. The key is always maintained valid without a dirty flag
         * to avoid condition check at runtime, considering these changes rarely.
         *
         * Only the toggle flags (depthWrite, depthTest) participate in the key. depthBias /
         * slopeDepthBias are runtime values applied via render-encoder state, not pipeline state,
         * so they do not need to disambiguate compiled pipelines.
         */
        uint32_t key() const { return _key; }

        bool depthWrite() const { return _depthWrite; }
        void setDepthWrite(bool value) {
            _depthWrite = value;
            _key = (_depthWrite ? 0u : 1u) | (_depthTest ? 0u : 2u);
        }

        bool depthTest() const { return _depthTest; }
        void setDepthTest(bool value) {
            _depthTest = value;
            _key = (_depthWrite ? 0u : 1u) | (_depthTest ? 0u : 2u);
        }

        // Constant depth bias added to each fragment's depth in hardware depth-buffer units.
        // Useful for decals and similar coplanar overlays to prevent z-fighting with the
        // surface they sit on. Negative values pull fragments toward the camera (in
        // reverse-Z, the engine's convention, "more positive depth" = closer; this matches
        // upstream Material.depthBias semantics where -0.1 nudges decals on top).
        // Combined with slopeDepthBias to handle slanted surfaces.
        float depthBias() const { return _depthBias; }
        void setDepthBias(float value) { _depthBias = value; }

        // Per-fragment depth bias scaled by the slope of the surface (max derivative of depth).
        // Important for decals on slanted surfaces where a constant bias is insufficient.
        // upstream calls this Material.slopeDepthBias.
        float slopeDepthBias() const { return _slopeDepthBias; }
        void setSlopeDepthBias(float value) { _slopeDepthBias = value; }

        bool hasDepthBias() const { return _depthBias != 0.0f || _slopeDepthBias != 0.0f; }

        /**
         * Create a DepthState with depth write disabled.
         * Useful for transparent materials that should not write to the depth buffer.
         */
        static DepthState noWrite() {
            DepthState state;
            state.setDepthWrite(false);
            return state;
        }

    private:
        uint32_t _key = 0;
        bool _depthWrite = true;
        bool _depthTest = true;
        float _depthBias = 0.0f;
        float _slopeDepthBias = 0.0f;
    };
}
