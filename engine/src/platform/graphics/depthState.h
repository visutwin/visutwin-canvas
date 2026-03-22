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
    };
}
