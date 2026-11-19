// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
#include "textureMips.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "core/math/color.h"

namespace visutwin::canvas
{
    namespace
    {
        // The linear value of each 8-bit sRGB code.
        const std::array<float, 256>& decodeTable()
        {
            static const std::array<float, 256> table = [] {
                std::array<float, 256> values{};
                for (int i = 0; i < 256; ++i) {
                    values[i] = gammaToLinear(static_cast<float>(i) / 255.0f);
                }
                return values;
            }();
            return table;
        }

        // The nearest 8-bit sRGB code of a linear value (rounded in the encoded domain).
        uint8_t encode(const float linear)
        {
            const float value = std::clamp(linear, 0.0f, 1.0f);
            return static_cast<uint8_t>(std::lround(linearToGamma(value) * 255.0f));
        }

        // The codes of the linear values k / kEncodeSteps: a linear step of 1.5e-5, under a
        // twentieth of the smallest step between two codes' linear values.
        constexpr int kEncodeSteps = 1 << 16;

        const std::vector<uint8_t>& encodeTable()
        {
            static const std::vector<uint8_t> table = [] {
                std::vector<uint8_t> values(kEncodeSteps + 1);
                for (int i = 0; i <= kEncodeSteps; ++i) {
                    values[i] = encode(static_cast<float>(i) / static_cast<float>(kEncodeSteps));
                }
                return values;
            }();
            return table;
        }
    }

    std::vector<std::vector<uint8_t>> generateMipChain8(const uint8_t* level0, const uint32_t width,
        const uint32_t height, const uint32_t channels, const bool srgb, const uint32_t levels)
    {
        std::vector<std::vector<uint8_t>> chain;
        if (!level0 || width == 0 || height == 0 || channels == 0 || channels > 4 || levels < 2) {
            return chain;
        }
        const auto& decode = decodeTable();
        const auto& encodeLut = encodeTable();
        const uint32_t colourChannels = srgb ? std::min(channels, 3u) : 0u;

        const uint8_t* source = level0;
        uint32_t sourceWidth = width;
        uint32_t sourceHeight = height;
        chain.reserve(levels - 1);
        for (uint32_t level = 1; level < levels; ++level) {
            const uint32_t w = std::max(1u, sourceWidth / 2);
            const uint32_t h = std::max(1u, sourceHeight / 2);
            std::vector<uint8_t> out(static_cast<size_t>(w) * h * channels);
            for (uint32_t y = 0; y < h; ++y) {
                const uint32_t y0 = std::min(2 * y, sourceHeight - 1);
                const uint32_t y1 = std::min(2 * y + 1, sourceHeight - 1);
                for (uint32_t x = 0; x < w; ++x) {
                    const uint32_t x0 = std::min(2 * x, sourceWidth - 1);
                    const uint32_t x1 = std::min(2 * x + 1, sourceWidth - 1);
                    const uint8_t* taps[4] = {
                        source + (static_cast<size_t>(y0) * sourceWidth + x0) * channels,
                        source + (static_cast<size_t>(y0) * sourceWidth + x1) * channels,
                        source + (static_cast<size_t>(y1) * sourceWidth + x0) * channels,
                        source + (static_cast<size_t>(y1) * sourceWidth + x1) * channels,
                    };
                    uint8_t* dst = out.data() + (static_cast<size_t>(y) * w + x) * channels;
                    for (uint32_t c = 0; c < channels; ++c) {
                        if (c < colourChannels) {
                            const float sum = decode[taps[0][c]] + decode[taps[1][c]] + decode[taps[2][c]] +
                                decode[taps[3][c]];
                            const float linear = std::clamp(sum * 0.25f, 0.0f, 1.0f);
                            dst[c] = encodeLut[static_cast<size_t>(std::lround(linear * kEncodeSteps))];
                        } else {
                            // A half rounds to even: rounding halves up would add an eighth of a
                            // count a level on average, a count over a deep chain.
                            const uint32_t sum = taps[0][c] + taps[1][c] + taps[2][c] + taps[3][c];
                            uint32_t q = sum / 4;
                            const uint32_t r = sum % 4;
                            if (r > 2 || (r == 2 && (q & 1u) != 0)) {
                                ++q;
                            }
                            dst[c] = static_cast<uint8_t>(q);
                        }
                    }
                }
            }
            chain.push_back(std::move(out));
            source = chain.back().data();
            sourceWidth = w;
            sourceHeight = h;
        }
        return chain;
    }
}
