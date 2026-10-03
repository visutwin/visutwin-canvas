// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 03.10.2026
//
// The sampler state a texture is read through, resolved from what the Texture carries
// (address modes, min and mag filters) and the device's anisotropy ratio.
//
// ONE mapping for both backends: Vulkan builds each texture's VkSampler from it
// (VulkanTexture::createSampler) and Metal keys its sampler-state cache on it
// (MetalSamplerCache), so a CLAMP_TO_EDGE atlas, a MIRRORED_REPEAT tile or a NEAREST
// pixel-art map wraps and filters the same on both. A mapping decided in one backend
// only would let the two drift apart again.
//
// What the state deliberately does NOT carry:
//   - The W address mode. Texture::setAddressW applies to volume textures only, no
//     material map is one, and every texture sampler is built with REPEAT on W.
//   - The mip half of the min filter. Every texture blends its mip levels linearly
//     (mipFilter is always Linear); the min filter picks only the filter within a
//     level. A texture with a single level samples the same either way.
//     DEVIATION: upstream's FILTER_NEAREST / FILTER_LINEAR also switch mip sampling
//     off, and its *_MIPMAP_NEAREST modes pick the nearest level.
//
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

#include "constants.h"
#include "texture.h"

namespace visutwin::canvas
{
    struct TextureSamplerState
    {
        enum class Filter : uint8_t
        {
            Nearest = 0,
            Linear = 1
        };

        AddressMode addressU = ADDRESS_REPEAT;
        AddressMode addressV = ADDRESS_REPEAT;
        Filter minFilter = Filter::Linear;
        Filter magFilter = Filter::Linear;
        Filter mipFilter = Filter::Linear;
        // 1 turns anisotropic filtering off. Never below 1.
        float maxAnisotropy = 1.0f;

        bool operator==(const TextureSamplerState&) const = default;

        /// Every field packed into one integer, for a cache key: two states have the
        /// same key exactly when they are equal (maxAnisotropy is never -0 or NaN).
        [[nodiscard]] uint64_t key() const
        {
            return (static_cast<uint64_t>(std::bit_cast<uint32_t>(maxAnisotropy)) << 32) |
                   static_cast<uint64_t>(static_cast<uint32_t>(addressU) & 0xFFu) |
                   (static_cast<uint64_t>(static_cast<uint32_t>(addressV) & 0xFFu) << 8) |
                   (static_cast<uint64_t>(minFilter) << 16) |
                   (static_cast<uint64_t>(magFilter) << 17) |
                   (static_cast<uint64_t>(mipFilter) << 18);
        }
    };

    /// The filter WITHIN a mip level that a FilterMode asks for.
    [[nodiscard]] constexpr TextureSamplerState::Filter textureSamplerFilter(const FilterMode mode)
    {
        switch (mode) {
        case FilterMode::FILTER_NEAREST:
        case FilterMode::FILTER_NEAREST_MIPMAP_NEAREST:
        case FilterMode::FILTER_NEAREST_MIPMAP_LINEAR:
            return TextureSamplerState::Filter::Nearest;
        default:
            return TextureSamplerState::Filter::Linear;
        }
    }

    /// The sampler state for a texture with these settings on a device whose samplers
    /// accept `deviceMaxAnisotropy`. Every texture gets the device's full anisotropy
    /// ratio: oblique ground textures otherwise smear into radial lines.
    [[nodiscard]] inline TextureSamplerState textureSamplerState(const AddressMode addressU,
        const AddressMode addressV, const FilterMode minFilter, const FilterMode magFilter,
        const float deviceMaxAnisotropy)
    {
        TextureSamplerState state;
        state.addressU = addressU;
        state.addressV = addressV;
        state.minFilter = textureSamplerFilter(minFilter);
        state.magFilter = textureSamplerFilter(magFilter);
        state.mipFilter = TextureSamplerState::Filter::Linear;
        state.maxAnisotropy = std::max(deviceMaxAnisotropy, 1.0f);
        return state;
    }

    [[nodiscard]] inline TextureSamplerState textureSamplerState(const Texture& texture,
        const float deviceMaxAnisotropy)
    {
        return textureSamplerState(texture.addressU(), texture.addressV(), texture.minFilter(),
            texture.magFilter(), deviceMaxAnisotropy);
    }

    /// The state of a texture left at its defaults (repeat, trilinear). Metal's default
    /// material sampler is created from exactly this, so a texture in its default state
    /// is read through that very sampler object.
    [[nodiscard]] inline TextureSamplerState defaultTextureSamplerState(const float deviceMaxAnisotropy)
    {
        return textureSamplerState(ADDRESS_REPEAT, ADDRESS_REPEAT, FilterMode::FILTER_LINEAR_MIPMAP_LINEAR,
            FilterMode::FILTER_LINEAR, deviceMaxAnisotropy);
    }
}
