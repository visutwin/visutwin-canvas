// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2025.
//
#pragma once

#include "core/utils.h"

namespace visutwin::canvas
{
    // Blend factor constants (indices into Metal blend factor array)
    constexpr int BLENDMODE_ZERO = 0;
    constexpr int BLENDMODE_ONE = 1;
    constexpr int BLENDMODE_SRC_COLOR = 2;
    constexpr int BLENDMODE_ONE_MINUS_SRC_COLOR = 3;
    constexpr int BLENDMODE_DST_COLOR = 4;
    constexpr int BLENDMODE_ONE_MINUS_DST_COLOR = 5;
    constexpr int BLENDMODE_SRC_ALPHA = 6;
    constexpr int BLENDMODE_SRC_ALPHA_SATURATE = 7;
    constexpr int BLENDMODE_ONE_MINUS_SRC_ALPHA = 8;
    constexpr int BLENDMODE_DST_ALPHA = 9;
    constexpr int BLENDMODE_ONE_MINUS_DST_ALPHA = 10;
    constexpr int BLENDMODE_CONSTANT = 11;
    constexpr int BLENDMODE_ONE_MINUS_CONSTANT = 12;

    // Blend equation constants
    constexpr int BLENDEQUATION_ADD = 0;
    constexpr int BLENDEQUATION_SUBTRACT = 1;
    constexpr int BLENDEQUATION_REVERSE_SUBTRACT = 2;
    constexpr int BLENDEQUATION_MIN = 3;
    constexpr int BLENDEQUATION_MAX = 4;

    /**
     * BlendState is a descriptor that defines how output of fragment shader is written and blended
     * into a render target. A blend state can be set on a material using Material::blendState,
     * or in some cases on the graphics device using GraphicsDevice::setBlendState.
     *
     * For the best performance, do not modify the blend state after it has been created, but create
     * multiple blend states and assign them to the material or graphics device as needed.
     */
    class BlendState
    {
    public:
        BlendState();

        uint32_t key() const {  return _target0.to_ulong(); }

        bool enabled() const;

        bool redWrite() const;
        bool greenWrite() const;
        bool blueWrite() const;
        bool alphaWrite() const;

        int colorOp() const;
        int colorSrcFactor() const;
        int colorDstFactor() const;

        int alphaOp() const;
        int alphaSrcFactor() const;
        int alphaDstFactor() const;

        // Setters for configuring blend state
        void setEnabled(bool value);
        void setColorOp(int op);
        void setColorSrcFactor(int factor);
        void setColorDstFactor(int factor);
        void setAlphaOp(int op);
        void setAlphaSrcFactor(int factor);
        void setAlphaDstFactor(int factor);
        void setRedWrite(bool value);
        void setGreenWrite(bool value);
        void setBlueWrite(bool value);
        void setAlphaWrite(bool value);

        // Factory for common blend modes

        // Standard alpha blending: src*srcAlpha + dst*(1-srcAlpha)
        static BlendState alphaBlend();

        // Multiplicative blending: dst * src (used by shadow catcher)
        static BlendState multiplicativeBlend();

        // Additive blending: src*srcAlpha + dst*ONE (particles glow and accumulate)
        static BlendState additiveBlend();

    private:
        // Bit field representing the blend state for render target 0
        BitPacking _target0;

        uint32_t getField(const BitPacking& bits, const int shift, const uint32_t mask) const {
            return ((bits.to_ulong() >> shift) & mask);
        }

        void setField(int shift, uint32_t mask, uint32_t value);
    };
}
