// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Per-pass texture binding deduplication.
// Extracted from MetalGraphicsDevice for single-responsibility decomposition.
//
#include "metalTextureBinder.h"

#include <cassert>
#include "metalTexture.h"
#include "platform/graphics/texture.h"
#include "scene/materials/material.h"

namespace visutwin::canvas
{
    // -----------------------------------------------------------------------
    // Low-level cached bind / clear
    // -----------------------------------------------------------------------

    void MetalTextureBinder::bindCached(MTL::RenderCommandEncoder* encoder, const int slot, Texture* texture)
    {
        assert(slot >= 0 && slot < kMaxTextureSlots);

        // Skip if the same texture is already bound at this slot.
        if (!_dirty && _boundTextures[slot] == texture) {
            return;
        }

        // Update the cache and perform the actual Metal bind.
        _boundTextures[slot] = texture;
        if (texture) {
            if (auto* hw = dynamic_cast<gpu::MetalTexture*>(texture->impl())) {
                if (hw->raw()) {
                    encoder->setFragmentTexture(hw->raw(), slot);
                    return;
                }
            }
        }
        encoder->setFragmentTexture(nullptr, slot);
    }

    void MetalTextureBinder::clearCached(MTL::RenderCommandEncoder* encoder, const int slot)
    {
        assert(slot >= 0 && slot < kMaxTextureSlots);

        if (!_dirty && _boundTextures[slot] == nullptr) {
            return;
        }

        _boundTextures[slot] = nullptr;
        encoder->setFragmentTexture(nullptr, slot);
    }

    // -----------------------------------------------------------------------
    // Sampler
    // -----------------------------------------------------------------------

    void MetalTextureBinder::bindSamplerCached(MTL::RenderCommandEncoder* encoder, MTL::SamplerState* sampler)
    {
        if (sampler && _boundSampler != sampler) {
            encoder->setFragmentSamplerState(sampler, 0);
            _boundSampler = sampler;
        }
    }

    // -----------------------------------------------------------------------
    // Material textures
    // -----------------------------------------------------------------------

    void MetalTextureBinder::bindMaterialTextures(MTL::RenderCommandEncoder* encoder,
        const std::vector<TextureSlot>& textureSlots)
    {
        // Clear material-owned slots (0,1,3,4,5) not used by this material.
        constexpr int materialSlots[] = {0, 1, 3, 4, 5, 17};
        for (const int s : materialSlots) {
            bool used = false;
            for (const auto& [slot, tex] : textureSlots) {
                if (slot == s) { used = true; break; }
            }
            if (!used) {
                clearCached(encoder, s);
            }
        }

        // Bind present material textures.
        for (const auto& [slot, texture] : textureSlots) {
            bindCached(encoder, slot, texture);
        }
    }

    void MetalTextureBinder::clearAllMaterialSlots(MTL::RenderCommandEncoder* encoder)
    {
        for (int i = 0; i < 8; ++i) {
            clearCached(encoder, i);
        }
    }

    // -----------------------------------------------------------------------
    // Scene-global textures
    // -----------------------------------------------------------------------

    void MetalTextureBinder::bindSceneTextures(MTL::RenderCommandEncoder* encoder,
        Texture* envAtlas, Texture* shadow, Texture* sceneDepth, Texture* skyboxCubeMap,
        Texture* reflection, Texture* reflectionDepth, Texture* ssao)
    {
        bindCached(encoder, 2, envAtlas);
        bindCached(encoder, 6, shadow);
        bindCached(encoder, 7, sceneDepth);
        bindCached(encoder, 8, skyboxCubeMap);
        bindCached(encoder, 9, reflection);
        bindCached(encoder, 10, reflectionDepth);
        bindCached(encoder, 18, ssao);
    }

    void MetalTextureBinder::bindQuadTextures(MTL::RenderCommandEncoder* encoder,
        const std::array<Texture*, 8>& quadBindings)
    {
        for (int i = 0; i < 8; ++i) {
            bindCached(encoder, i, quadBindings[i]);
        }
    }

    // -----------------------------------------------------------------------
    // Local shadow textures
    // -----------------------------------------------------------------------

    void MetalTextureBinder::bindLocalShadowTextures(MTL::RenderCommandEncoder* encoder,
        Texture* shadow0, Texture* shadow1)
    {
        bindCached(encoder, 11, shadow0);
        bindCached(encoder, 12, shadow1);
    }

    void MetalTextureBinder::bindOmniShadowTextures(MTL::RenderCommandEncoder* encoder,
        Texture* cube0, Texture* cube1)
    {
        bindCached(encoder, 15, cube0);
        bindCached(encoder, 16, cube1);
    }

    // -----------------------------------------------------------------------
    // Pass lifecycle
    // -----------------------------------------------------------------------

    void MetalTextureBinder::resetPassState()
    {
        _boundTextures.fill(nullptr);
        _dirty = true;
        _boundSampler = nullptr;
    }
}
