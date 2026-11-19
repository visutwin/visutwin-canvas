// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
// IEEE half floats, as 16 bits, for texel data and serialized data: floatToHalf rounds to the
// nearest half, a tie away from zero (to even only between 2^-25 and 2^-24), which is how the
// probe volumes upstream saves were rounded; halfToFloat is exact.
//
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace visutwin::canvas
{
    /// The half float nearest a float: rounded, with the overflow to infinity and NaN kept.
    inline uint16_t floatToHalf(const float value)
    {
        const auto x = static_cast<int32_t>(std::bit_cast<uint32_t>(value));

        uint32_t bits = static_cast<uint32_t>(x >> 16) & 0x8000u;   // the sign
        uint32_t m = static_cast<uint32_t>(x >> 12) & 0x07ffu;      // one extra bit for rounding
        const int32_t e = (x >> 23) & 0xff;

        // Zero, a denormal, or too small for a half denormal: a signed zero. Between 2^-25 and
        // 2^-24 it rounds up to the smallest denormal; exactly 2^-25 is a tie to even (zero).
        if (e < 103) {
            if (e == 102 && (x & 0x007fffff) != 0) {
                bits |= 1u;
            }
            return static_cast<uint16_t>(bits);
        }

        // NaN stays NaN; infinity, or too large, is infinity.
        if (e > 142) {
            bits |= 0x7c00u;
            if (e == 255 && (x & 0x007fffff) != 0) {
                bits |= 0x0200u;
            }
            return static_cast<uint16_t>(bits);
        }

        // Too small for a normal half: a denormal (the rounding may carry into the exponent).
        if (e < 113) {
            m |= 0x0800u;
            bits |= (m >> (114 - e)) + ((m >> (113 - e)) & 1u);
            return static_cast<uint16_t>(bits);
        }

        bits |= (static_cast<uint32_t>(e - 112) << 10) | (m >> 1);
        // The rounding may carry into the exponent, which is right.
        bits += m & 1u;
        return static_cast<uint16_t>(bits);
    }

    /// The float a half float stores.
    inline float halfToFloat(const uint16_t half)
    {
        const float sign = (half & 0x8000u) != 0 ? -1.0f : 1.0f;
        const int exponent = (half >> 10) & 0x1f;
        const int mantissa = half & 0x3ff;
        if (exponent == 0) {
            return sign * std::ldexp(static_cast<float>(mantissa), -24);
        }
        if (exponent == 31) {
            return mantissa != 0 ? std::numeric_limits<float>::quiet_NaN() : sign * std::numeric_limits<float>::infinity();
        }
        return sign * std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f, exponent - 15);
    }
}
