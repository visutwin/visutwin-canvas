// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 03.10.2026
//
// Metal sampler states keyed on TextureSamplerState, so a material map is read through
// the sampler its own Texture describes rather than one shared repeat sampler.
//
#pragma once

#include <cstdint>
#include <unordered_map>

#include <Metal/Metal.hpp>

#include "platform/graphics/textureSamplerState.h"

namespace visutwin::canvas
{
    class Texture;

    /**
     * Creates each distinct sampler state once and owns it until release(). The number
     * of distinct states is tiny (three address modes per axis, two filters each for min
     * and mag), so nothing is ever evicted, and a pointer handed out stays valid for the
     * device's lifetime.
     *
     * The default material sampler is created through the cache too, from
     * defaultTextureSamplerState(), so a texture whose state is the default is read
     * through that very object.
     */
    class MetalSamplerCache
    {
    public:
        MetalSamplerCache() = default;
        ~MetalSamplerCache();

        MetalSamplerCache(const MetalSamplerCache&) = delete;
        MetalSamplerCache& operator=(const MetalSamplerCache&) = delete;

        /// Binds the cache to a device and creates the default sampler. maxAnisotropy is
        /// the device's published ratio, the one every texture sampler uses.
        void init(MTL::Device* device, float maxAnisotropy);

        /// Releases every sampler state. Call before the device goes away.
        void release();

        /// The sampler for a texture left at its defaults: repeat, trilinear, anisotropic.
        [[nodiscard]] MTL::SamplerState* defaultSampler() const { return _defaultSampler; }

        /// The sampler for `texture`'s own state; the default sampler for null.
        [[nodiscard]] MTL::SamplerState* forTexture(const Texture* texture);

        /// The sampler for `state`, created on first request.
        [[nodiscard]] MTL::SamplerState* get(const TextureSamplerState& state);

    private:
        MTL::Device* _device = nullptr;
        float _maxAnisotropy = 1.0f;
        MTL::SamplerState* _defaultSampler = nullptr;
        std::unordered_map<uint64_t, MTL::SamplerState*> _samplers;
    };
}
