// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2025.
//
#include "blendState.h"

namespace visutwin::canvas
{
    // masks (to only keep relevant bits)
    const uint32_t opMask = 0b111;
    const uint32_t factorMask = 0b1111;

    // Shifts values to where individual parts are stored
    constexpr uint32_t colorOpShift = 0;             // 00 - 02 (3bits)
    constexpr uint32_t colorSrcFactorShift = 3;      // 03 - 06 (4bits)
    constexpr uint32_t colorDstFactorShift = 7;      // 07 - 10 (4bits)
    constexpr uint32_t alphaOpShift = 11;            // 11 - 13 (3bits)
    constexpr uint32_t alphaSrcFactorShift = 14;     // 14 - 17 (4bits)
    constexpr uint32_t alphaDstFactorShift = 18;     // 18 - 21 (4bits)
    constexpr uint32_t redWriteShift = 22;           // 22 (1 bit)
    constexpr uint32_t greenWriteShift = 23;         // 23 (1 bit)
    constexpr uint32_t blueWriteShift = 24;          // 24 (1 bit)
    constexpr uint32_t alphaWriteShift = 25;         // 25 (1 bit)
    constexpr uint32_t blendShift = 26;              // 26 (1 bit)

    BlendState::BlendState()
    {
        // JS default equivalent: color writes enabled, blending disabled.
        _target0.set(redWriteShift);
        _target0.set(greenWriteShift);
        _target0.set(blueWriteShift);
        _target0.set(alphaWriteShift);
    }

    bool BlendState::enabled() const
    {
        return _target0.test(blendShift);
    }

    bool BlendState::redWrite() const
    {
        return _target0.test(redWriteShift);
    }

    bool BlendState::greenWrite() const
    {
        return _target0.test(greenWriteShift);
    }

    bool BlendState::blueWrite() const
    {
        return _target0.test(blueWriteShift);
    }

    bool BlendState::alphaWrite() const
    {
        return _target0.test(alphaWriteShift);
    }

    int BlendState::colorOp() const
    {
        return getField(_target0, colorOpShift, opMask);
    }

    int BlendState::colorSrcFactor() const
    {
        return getField(_target0, colorSrcFactorShift, factorMask);
    }

    int BlendState::colorDstFactor() const
    {
        return getField(_target0, colorDstFactorShift, factorMask);
    }

    int BlendState::alphaOp() const
    {
        return getField(_target0, alphaOpShift, opMask);
    }

    int BlendState::alphaSrcFactor() const
    {
        return getField(_target0, alphaSrcFactorShift, factorMask);
    }

    int BlendState::alphaDstFactor() const
    {
        return getField(_target0, alphaDstFactorShift, factorMask);
    }

    // --- Setters ---

    void BlendState::setField(int shift, uint32_t mask, uint32_t value)
    {
        // Clear the bits in the field, then set the new value
        const int width = __builtin_popcount(mask);
        for (int i = 0; i < width; ++i) {
            _target0.reset(shift + i);
        }
        for (int i = 0; i < width; ++i) {
            if (value & (1u << i)) {
                _target0.set(shift + i);
            }
        }
    }

    void BlendState::setEnabled(bool value)
    {
        if (value) {
            _target0.set(blendShift);
        } else {
            _target0.reset(blendShift);
        }
    }

    void BlendState::setColorOp(int op)
    {
        setField(colorOpShift, opMask, static_cast<uint32_t>(op));
    }

    void BlendState::setColorSrcFactor(int factor)
    {
        setField(colorSrcFactorShift, factorMask, static_cast<uint32_t>(factor));
    }

    void BlendState::setColorDstFactor(int factor)
    {
        setField(colorDstFactorShift, factorMask, static_cast<uint32_t>(factor));
    }

    void BlendState::setAlphaOp(int op)
    {
        setField(alphaOpShift, opMask, static_cast<uint32_t>(op));
    }

    void BlendState::setAlphaSrcFactor(int factor)
    {
        setField(alphaSrcFactorShift, factorMask, static_cast<uint32_t>(factor));
    }

    void BlendState::setAlphaDstFactor(int factor)
    {
        setField(alphaDstFactorShift, factorMask, static_cast<uint32_t>(factor));
    }

    void BlendState::setRedWrite(bool value)
    {
        if (value) _target0.set(redWriteShift); else _target0.reset(redWriteShift);
    }

    void BlendState::setGreenWrite(bool value)
    {
        if (value) _target0.set(greenWriteShift); else _target0.reset(greenWriteShift);
    }

    void BlendState::setBlueWrite(bool value)
    {
        if (value) _target0.set(blueWriteShift); else _target0.reset(blueWriteShift);
    }

    void BlendState::setAlphaWrite(bool value)
    {
        if (value) _target0.set(alphaWriteShift); else _target0.reset(alphaWriteShift);
    }

    // --- Factory methods ---

    BlendState BlendState::alphaBlend()
    {
        BlendState state;
        state.setEnabled(true);
        state.setColorOp(BLENDEQUATION_ADD);
        state.setColorSrcFactor(BLENDMODE_SRC_ALPHA);
        state.setColorDstFactor(BLENDMODE_ONE_MINUS_SRC_ALPHA);
        state.setAlphaOp(BLENDEQUATION_ADD);
        state.setAlphaSrcFactor(BLENDMODE_ONE);
        state.setAlphaDstFactor(BLENDMODE_ONE_MINUS_SRC_ALPHA);
        return state;
    }

    BlendState BlendState::multiplicativeBlend()
    {
        // BLEND_MULTIPLICATIVE: output.rgb * framebuffer.rgb
        // srcFactor = DST_COLOR, dstFactor = ZERO, op = ADD
        // Result: src * dst + dst * 0 = src * dst
        BlendState state;
        state.setEnabled(true);
        state.setColorOp(BLENDEQUATION_ADD);
        state.setColorSrcFactor(BLENDMODE_DST_COLOR);
        state.setColorDstFactor(BLENDMODE_ZERO);
        state.setAlphaOp(BLENDEQUATION_ADD);
        state.setAlphaSrcFactor(BLENDMODE_ONE);
        state.setAlphaDstFactor(BLENDMODE_ZERO);
        return state;
    }

    BlendState BlendState::additiveBlend()
    {
        // BLEND_ADDITIVE: particles glow and accumulate light.
        // srcFactor = SRC_ALPHA, dstFactor = ONE, op = ADD
        // Result: src * srcAlpha + dst * 1 = premultiplied src + dst
        BlendState state;
        state.setEnabled(true);
        state.setColorOp(BLENDEQUATION_ADD);
        state.setColorSrcFactor(BLENDMODE_SRC_ALPHA);
        state.setColorDstFactor(BLENDMODE_ONE);
        state.setAlphaOp(BLENDEQUATION_ADD);
        state.setAlphaSrcFactor(BLENDMODE_ONE);
        state.setAlphaDstFactor(BLENDMODE_ONE);
        return state;
    }
}
