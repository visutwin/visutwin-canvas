// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 03.10.2026
//
// textureSamplerState is the one mapping from a Texture's wrap and filter settings to
// the sampler it is read through: Vulkan builds each texture's VkSampler from it and
// Metal keys its sampler-state cache on it. These checks pin the mapping and the key
// without a GPU.
//
// Three properties matter. A texture left at its defaults must resolve to EXACTLY the
// default state, because Metal reads such a texture through its default sampler object
// and a near miss would create a second sampler and change no pixel, hiding a mapping
// drift. Every field must reach the key, or two textures that wrap differently share a
// cached sampler. And the mapping must be the one the Vulkan backend always applied:
// filter within a level from the min/mag mode, linear mips whatever the mode says, the
// device's full anisotropy.

#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <string>

#include "platform/graphics/texture.h"
#include "platform/graphics/textureSamplerState.h"
#include "support/check.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    using Filter = TextureSamplerState::Filter;

    constexpr float kAnisotropy = 16.0f;
}

int main()
{
    std::cout << std::unitbuf;

    auto device = std::make_shared<StubGraphicsDevice>();

    std::cout << "a texture at its defaults resolves to the default state\n";
    {
        Texture texture(device.get());
        const TextureSamplerState state = textureSamplerState(texture, kAnisotropy);
        const TextureSamplerState expected = defaultTextureSamplerState(kAnisotropy);
        check(state == expected, "equal to defaultTextureSamplerState");
        check(state.key() == expected.key(), "and the same cache key");
        check(state.addressU == ADDRESS_REPEAT && state.addressV == ADDRESS_REPEAT, "repeat on U and V");
        check(state.minFilter == Filter::Linear && state.magFilter == Filter::Linear, "linear min and mag");
        check(state.mipFilter == Filter::Linear, "linear mips");
        check(state.maxAnisotropy == kAnisotropy, "the device's anisotropy ratio");
    }

    std::cout << "a texture created without mips, or with a linear min filter, is still the default\n";
    {
        TextureOptions options;
        options.mipmaps = false;
        options.minFilter = FilterMode::FILTER_LINEAR;
        Texture texture(device.get(), options);
        check(textureSamplerState(texture, kAnisotropy) == defaultTextureSamplerState(kAnisotropy),
            "FILTER_LINEAR blends mips linearly, as every texture does");
    }

    std::cout << "the filter within a level follows the mode's first half\n";
    {
        struct Case
        {
            FilterMode mode;
            Filter expected;
            const char* name;
        };
        const Case cases[] = {
            {FilterMode::FILTER_NEAREST, Filter::Nearest, "FILTER_NEAREST"},
            {FilterMode::FILTER_LINEAR, Filter::Linear, "FILTER_LINEAR"},
            {FilterMode::FILTER_NEAREST_MIPMAP_NEAREST, Filter::Nearest, "FILTER_NEAREST_MIPMAP_NEAREST"},
            {FilterMode::FILTER_NEAREST_MIPMAP_LINEAR, Filter::Nearest, "FILTER_NEAREST_MIPMAP_LINEAR"},
            {FilterMode::FILTER_LINEAR_MIPMAP_NEAREST, Filter::Linear, "FILTER_LINEAR_MIPMAP_NEAREST"},
            {FilterMode::FILTER_LINEAR_MIPMAP_LINEAR, Filter::Linear, "FILTER_LINEAR_MIPMAP_LINEAR"},
        };
        for (const Case& c : cases) {
            const TextureSamplerState asMin = textureSamplerState(ADDRESS_REPEAT, ADDRESS_REPEAT, c.mode,
                FilterMode::FILTER_LINEAR, kAnisotropy);
            const TextureSamplerState asMag = textureSamplerState(ADDRESS_REPEAT, ADDRESS_REPEAT,
                FilterMode::FILTER_LINEAR_MIPMAP_LINEAR, c.mode, kAnisotropy);
            check(asMin.minFilter == c.expected && asMin.magFilter == Filter::Linear,
                std::string(c.name) + " as the min filter");
            check(asMag.magFilter == c.expected && asMag.minFilter == Filter::Linear,
                std::string(c.name) + " as the mag filter");
            check(asMin.mipFilter == Filter::Linear && asMag.mipFilter == Filter::Linear,
                std::string(c.name) + " leaves the mips linear");
        }
    }

    std::cout << "the texture's own setters reach the state\n";
    {
        Texture texture(device.get());
        texture.setAddressU(ADDRESS_CLAMP_TO_EDGE);
        texture.setAddressV(ADDRESS_MIRRORED_REPEAT);
        texture.setMinFilter(FilterMode::FILTER_NEAREST_MIPMAP_NEAREST);
        texture.setMagFilter(FilterMode::FILTER_NEAREST);
        const TextureSamplerState state = textureSamplerState(texture, kAnisotropy);
        check(state.addressU == ADDRESS_CLAMP_TO_EDGE, "address U clamp-to-edge");
        check(state.addressV == ADDRESS_MIRRORED_REPEAT, "address V mirrored repeat");
        check(state.minFilter == Filter::Nearest && state.magFilter == Filter::Nearest, "nearest min and mag");
        check(!(state == defaultTextureSamplerState(kAnisotropy)), "no longer the default state");
    }

    std::cout << "every combination has its own key\n";
    {
        const AddressMode modes[] = {ADDRESS_REPEAT, ADDRESS_CLAMP_TO_EDGE, ADDRESS_MIRRORED_REPEAT};
        const FilterMode filters[] = {FilterMode::FILTER_NEAREST, FilterMode::FILTER_LINEAR};
        const float anisotropies[] = {1.0f, 4.0f, 16.0f};
        std::set<uint64_t> keys;
        int combinations = 0;
        for (const AddressMode u : modes) {
            for (const AddressMode v : modes) {
                for (const FilterMode minFilter : filters) {
                    for (const FilterMode magFilter : filters) {
                        for (const float anisotropy : anisotropies) {
                            keys.insert(textureSamplerState(u, v, minFilter, magFilter, anisotropy).key());
                            ++combinations;
                        }
                    }
                }
            }
        }
        check(static_cast<int>(keys.size()) == combinations, "3 x 3 x 2 x 2 x 3 states, as many keys");

        TextureSamplerState nearestMips = defaultTextureSamplerState(kAnisotropy);
        nearestMips.mipFilter = Filter::Nearest;
        check(nearestMips.key() != defaultTextureSamplerState(kAnisotropy).key(), "the mip filter reaches the key");
    }

    std::cout << "anisotropy is the device's ratio, never below 1\n";
    {
        check(textureSamplerState(ADDRESS_REPEAT, ADDRESS_REPEAT, FilterMode::FILTER_LINEAR,
                  FilterMode::FILTER_LINEAR, 0.0f).maxAnisotropy == 1.0f,
            "a ratio of 0 is 1 (anisotropic filtering off)");
        check(textureSamplerState(ADDRESS_REPEAT, ADDRESS_REPEAT, FilterMode::FILTER_LINEAR,
                  FilterMode::FILTER_LINEAR, 8.0f).maxAnisotropy == 8.0f,
            "a ratio of 8 is passed through");
        check(defaultTextureSamplerState(1.0f).key() != defaultTextureSamplerState(kAnisotropy).key(),
            "the ratio reaches the key");
    }

    return finish("texture-sampler-state");
}
