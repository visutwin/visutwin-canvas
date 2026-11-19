// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
// The mip chains an 8-bit 2D texture gets on the CPU (platform/graphics/textureMips.h,
// Texture::upload): sizes, the exact 2x2 average, a mean that does not drift down a deep chain
// (the GPU generators it replaces moved a 2048 texture's mean by two counts), the sRGB colour
// averaged in linear light with alpha as stored, odd sizes, and the Texture rules: a chain is
// built only for an 8-bit 2D texture asking for mipmaps with level 0 alone, rebuilt when level 0
// changes, and never over levels the caller supplied.

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <random>
#include <vector>

#include "core/math/color.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/textureMips.h"
#include "support/check.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    double mean(const std::vector<uint8_t>& texels, const uint32_t channels, const uint32_t channel)
    {
        double sum = 0.0;
        for (size_t i = channel; i < texels.size(); i += channels) {
            sum += texels[i];
        }
        return sum / static_cast<double>(texels.size() / channels);
    }

    double linearMean(const std::vector<uint8_t>& texels, const uint32_t channels, const uint32_t channel)
    {
        double sum = 0.0;
        for (size_t i = channel; i < texels.size(); i += channels) {
            sum += gammaToLinear(static_cast<float>(texels[i]) / 255.0f);
        }
        return sum / static_cast<double>(texels.size() / channels);
    }
}

int main()
{
    std::cout << std::unitbuf;

    std::cout << "the chain\n";
    {
        // 4 x 2, one channel: 0 10 20 30 / 40 50 60 70
        const std::vector<uint8_t> top = {0, 10, 20, 30, 40, 50, 60, 70};
        const auto chain = generateMipChain8(top.data(), 4, 2, 1, false, 3);
        check(chain.size() == 2 && chain[0].size() == 2 && chain[1].size() == 1, "halves each axis, never below 1");
        check(chain[0][0] == 25 && chain[0][1] == 45, "a texel is the average of its 2 x 2 block");
        check(chain[1][0] == 35, "and the next level averages the one above");
        const std::vector<uint8_t> half = {1, 2, 2, 3};   // sum 8: 2 exactly; {1,1,1,2}: 1.25
        check(generateMipChain8(half.data(), 2, 2, 1, false, 2)[0][0] == 2, "an exact average stays");
        const std::vector<uint8_t> tie = {1, 1, 2, 2};     // 1.5 -> 2 (even)
        const std::vector<uint8_t> tie2 = {2, 2, 3, 3};    // 2.5 -> 2 (even)
        check(generateMipChain8(tie.data(), 2, 2, 1, false, 2)[0][0] == 2 &&
            generateMipChain8(tie2.data(), 2, 2, 1, false, 2)[0][0] == 2, "a half rounds to even");
        const std::vector<uint8_t> odd = {10, 20, 90, 30, 40, 90, 90, 90, 90};   // 3 x 3
        const auto oddChain = generateMipChain8(odd.data(), 3, 3, 1, false, 2);
        check(oddChain.size() == 1 && oddChain[0].size() == 1 && oddChain[0][0] == 25,
            "an odd size takes the top-left block, as a box downsample does");
        check(generateMipChain8(top.data(), 4, 2, 1, false, 1).empty() &&
            generateMipChain8(nullptr, 4, 2, 1, false, 3).empty(), "no chain for one level or no data");
    }

    std::cout << "the mean down a deep chain\n";
    {
        std::mt19937 random(7);
        std::uniform_int_distribution<int> noise(-40, 40);
        const uint32_t size = 1024;
        std::vector<uint8_t> top(static_cast<size_t>(size) * size * 4);
        for (size_t i = 0; i < top.size(); i += 4) {
            top[i] = static_cast<uint8_t>(std::clamp(190 + noise(random), 0, 255));
            top[i + 1] = static_cast<uint8_t>(std::clamp(60 + noise(random), 0, 255));
            top[i + 2] = static_cast<uint8_t>(std::clamp(120 + noise(random), 0, 255));
            top[i + 3] = static_cast<uint8_t>(std::clamp(128 + noise(random), 0, 255));
        }
        const auto chain = generateMipChain8(top.data(), size, size, 4, false, 11);
        check(chain.size() == 10 && chain.back().size() == 4, "a 1024 chain ends at 1 x 1");
        bool kept = true;
        for (uint32_t c = 0; c < 4; ++c) {
            kept = kept && std::abs(chain.back()[c] - mean(top, 4, c)) <= 0.6;
        }
        check(kept, "as stored, the 1 x 1 is the top level's mean, within rounding");

        const auto srgbChain = generateMipChain8(top.data(), size, size, 4, true, 11);
        bool linear = true;
        for (uint32_t c = 0; c < 3; ++c) {
            const float expected = linearToGamma(static_cast<float>(linearMean(top, 4, c))) * 255.0f;
            linear = linear && std::abs(srgbChain.back()[c] - expected) <= 0.6f;
        }
        check(linear, "sRGB colour: the 1 x 1 is the top level's mean in linear light");
        check(srgbChain.back()[3] == chain.back()[3], "and its alpha is averaged as stored");
        check(srgbChain.back()[0] > chain.back()[0], "which is brighter than the mean of the codes");
    }

    std::cout << "Texture::upload\n";
    {
        const auto device = std::make_shared<StubGraphicsDevice>();
        const auto make = [&](const PixelFormat format, const bool mipmaps, const bool srgb) {
            TextureOptions options;
            options.width = 4;
            options.height = 4;
            options.format = format;
            options.mipmaps = mipmaps;
            options.srgb = srgb;
            return std::make_unique<Texture>(device.get(), options);
        };
        std::vector<uint8_t> grey(4 * 4 * 4, 100);
        auto texture = make(PixelFormat::PIXELFORMAT_RGBA8, true, false);
        texture->setLevelData(0, grey.data(), grey.size());
        texture->upload();
        check(texture->getLevel(1) && texture->getLevel(2), "an 8-bit mipmapped texture gets its chain on the CPU");
        check(texture->getLevel(2) && static_cast<const uint8_t*>(texture->getLevel(2))[0] == 100, "of its level 0");

        std::vector<uint8_t> white(4 * 4 * 4, 200);
        texture->setLevelData(0, white.data(), white.size());
        texture->upload();
        check(static_cast<const uint8_t*>(texture->getLevel(2))[0] == 200, "rebuilt when level 0 changes");

        auto flat = make(PixelFormat::PIXELFORMAT_RGBA8, false, false);
        flat->setLevelData(0, grey.data(), grey.size());
        flat->upload();
        check(!flat->getLevel(1), "none without mipmaps");

        auto own = make(PixelFormat::PIXELFORMAT_RGBA8, true, false);
        own->setLevelData(0, grey.data(), grey.size());
        std::vector<uint8_t> level1(2 * 2 * 4, 7);
        own->setLevelData(1, level1.data(), level1.size());
        own->upload();
        check(static_cast<const uint8_t*>(own->getLevel(1))[0] == 7 && !own->getLevel(2),
            "and none over levels the caller supplied");

        auto half = make(PixelFormat::PIXELFORMAT_RGBA16F, true, false);
        std::vector<uint8_t> halves(4 * 4 * 8, 0);
        half->setLevelData(0, halves.data(), halves.size());
        half->upload();
        check(!half->getLevel(1), "a float texture keeps the GPU's chain");

        auto colour = make(PixelFormat::PIXELFORMAT_RGBA8, true, true);
        std::vector<uint8_t> checker(4 * 4 * 4);
        for (size_t i = 0; i < 16; ++i) {
            const uint8_t v = ((i % 4) + (i / 4)) % 2 == 0 ? 0 : 255;
            checker[i * 4] = checker[i * 4 + 1] = checker[i * 4 + 2] = v;
            checker[i * 4 + 3] = v;
        }
        colour->setLevelData(0, checker.data(), checker.size());
        colour->upload();
        const auto* level1Colour = static_cast<const uint8_t*>(colour->getLevel(1));
        check(level1Colour && level1Colour[0] == static_cast<uint8_t>(std::lround(linearToGamma(0.5f) * 255.0f)) &&
            level1Colour[3] == 128, "an sRGB texture: black and white average to linear 0.5, alpha to 128");
    }

    return finish("texture-mips");
}
