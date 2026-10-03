// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 03.10.2026
//
#include "metalSamplerCache.h"

#include <spdlog/spdlog.h>

#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    namespace
    {
        MTL::SamplerAddressMode toMetalAddressMode(const AddressMode mode)
        {
            switch (mode) {
            case ADDRESS_CLAMP_TO_EDGE:
                return MTL::SamplerAddressModeClampToEdge;
            case ADDRESS_MIRRORED_REPEAT:
                return MTL::SamplerAddressModeMirrorRepeat;
            default:
                return MTL::SamplerAddressModeRepeat;
            }
        }

        MTL::SamplerMinMagFilter toMetalMinMagFilter(const TextureSamplerState::Filter filter)
        {
            return filter == TextureSamplerState::Filter::Nearest
                ? MTL::SamplerMinMagFilterNearest : MTL::SamplerMinMagFilterLinear;
        }

        MTL::SamplerMipFilter toMetalMipFilter(const TextureSamplerState::Filter filter)
        {
            return filter == TextureSamplerState::Filter::Nearest
                ? MTL::SamplerMipFilterNearest : MTL::SamplerMipFilterLinear;
        }
    }

    MetalSamplerCache::~MetalSamplerCache()
    {
        release();
    }

    void MetalSamplerCache::init(MTL::Device* device, const float maxAnisotropy)
    {
        release();
        _device = device;
        _maxAnisotropy = maxAnisotropy;
        _defaultSampler = get(defaultTextureSamplerState(maxAnisotropy));
    }

    void MetalSamplerCache::release()
    {
        for (const auto& entry : _samplers) {
            if (entry.second) {
                entry.second->release();
            }
        }
        _samplers.clear();
        _defaultSampler = nullptr;
    }

    MTL::SamplerState* MetalSamplerCache::forTexture(const Texture* texture)
    {
        if (!texture) {
            return _defaultSampler;
        }
        return get(textureSamplerState(*texture, _maxAnisotropy));
    }

    MTL::SamplerState* MetalSamplerCache::get(const TextureSamplerState& state)
    {
        const uint64_t key = state.key();
        if (const auto it = _samplers.find(key); it != _samplers.end()) {
            return it->second;
        }
        if (!_device) {
            return _defaultSampler;
        }

        // Only S and T are set: the R address mode stays at Metal's default, as the
        // default sampler has always had it (see TextureSamplerState on why W is not
        // part of the state). Every other descriptor field is Metal's default too —
        // normalized coordinates, LOD range [0, FLT_MAX], no compare function.
        auto* descriptor = MTL::SamplerDescriptor::alloc()->init();
        descriptor->setMinFilter(toMetalMinMagFilter(state.minFilter));
        descriptor->setMagFilter(toMetalMinMagFilter(state.magFilter));
        descriptor->setMipFilter(toMetalMipFilter(state.mipFilter));
        descriptor->setMaxAnisotropy(static_cast<NS::UInteger>(state.maxAnisotropy));
        descriptor->setSAddressMode(toMetalAddressMode(state.addressU));
        descriptor->setTAddressMode(toMetalAddressMode(state.addressV));
        MTL::SamplerState* sampler = _device->newSamplerState(descriptor);
        descriptor->release();

        if (!sampler) {
            spdlog::error("MetalSamplerCache: failed to create a sampler state");
            return _defaultSampler;
        }
        _samplers.emplace(key, sampler);
        return sampler;
    }
}
