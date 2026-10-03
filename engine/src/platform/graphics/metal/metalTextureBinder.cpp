// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// Per-pass texture binding deduplication.
// Extracted from MetalGraphicsDevice for single-responsibility decomposition.
//
#include "metalTextureBinder.h"

#include <cassert>
#include "metalSamplerCache.h"
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

    void MetalTextureBinder::bindSamplerCached(MTL::RenderCommandEncoder* encoder, const int slot,
        MTL::SamplerState* sampler)
    {
        assert(slot >= 0 && slot < kSamplerSlotCount);
        if (sampler && _boundSamplers[slot] != sampler) {
            encoder->setFragmentSamplerState(sampler, static_cast<NS::UInteger>(slot));
            _boundSamplers[slot] = sampler;
        }
    }

    void MetalTextureBinder::bindDrawSamplers(MTL::RenderCommandEncoder* encoder, MTL::SamplerState* general,
        MTL::SamplerState* materialDefault, const bool materialSamplers)
    {
        bindSamplerCached(encoder, 0, general);
        if (!materialSamplers) {
            return;
        }
        for (int i = 0; i < kMaterialSamplerCount; ++i) {
            MTL::SamplerState* sampler = _materialSamplers[i] ? _materialSamplers[i] : materialDefault;
            bindSamplerCached(encoder, kFirstMaterialSamplerSlot + i, sampler);
        }
    }

    // -----------------------------------------------------------------------
    // Material textures
    // -----------------------------------------------------------------------

    static_assert(MetalTextureBinder::kMaterialSamplerTextureSlots[5] == kLightMapTextureSlot,
        "the last material sampler slot follows the lightmap");

    void MetalTextureBinder::bindMaterialTextures(MTL::RenderCommandEncoder* encoder,
        const std::vector<TextureSlot>& textureSlots, MetalSamplerCache& samplers)
    {
        // Each material sampler slot takes the sampler of the texture at its material
        // slot, so the map is read with its own wrap and filter. A slot with no texture
        // records null and is bound the default sampler; a texture in the default state
        // resolves to that same sampler object, so it samples exactly as it always did.
        for (int i = 0; i < kMaterialSamplerCount; ++i) {
            const Texture* texture = nullptr;
            for (const auto& [slot, tex] : textureSlots) {
                if (slot == kMaterialSamplerTextureSlots[i]) {
                    texture = tex;
                    break;
                }
            }
            _materialSamplers[i] = texture ? samplers.forTexture(texture) : nullptr;
        }

        // Clear material-owned slots not used by this material.
        constexpr int materialSlots[] = {0, 1, 3, 4, 5, 7, 13, 14, 17, 19, 23, 31, 32, 33, 34};
        for (const int s : materialSlots) {
            bool used = false;
            for (const auto& [slot, tex] : textureSlots) {
                if (slot == s) { used = true; break; }
            }
            if (!used) {
                clearCached(encoder, s);
            }
        }

        // Bind present material textures. Slots >= 100 are VERTEX-stage textures
        // (slot - 100), used by displacement mapping.
        bool vertexSlot0Used = false;
        for (const auto& [slot, texture] : textureSlots) {
            if (slot >= 100) {
                if (texture) {
                    if (auto* hw = dynamic_cast<gpu::MetalTexture*>(texture->impl()); hw && hw->raw()) {
                        encoder->setVertexTexture(hw->raw(), static_cast<NS::UInteger>(slot - 100));
                        vertexSlot0Used = (slot == 100);
                    }
                }
                continue;
            }
            bindCached(encoder, slot, texture);
        }
        if (!vertexSlot0Used) {
            encoder->setVertexTexture(nullptr, 0);
        }
    }

    void MetalTextureBinder::clearMaterialSlots(MTL::RenderCommandEncoder* encoder)
    {
        // The material's slots among 0-7, NOT 2 and 6: those are the environment atlas
        // and the shadow map, which bindSceneTextures binds right after this for the same
        // draw. Clearing them here cost a clear and a rebind of
        // each on every material-less draw — every opaque shadow caster.
        constexpr int materialSlots[] = {0, 1, 3, 4, 5, 7};
        for (const int slot : materialSlots) {
            clearCached(encoder, slot);
        }
        // No material, no maps: their sampler slots go back to the default sampler, so a
        // later draw cannot read a map through the previous material's sampler.
        _materialSamplers.fill(nullptr);
    }

    // -----------------------------------------------------------------------
    // Scene-global textures
    // -----------------------------------------------------------------------

    void MetalTextureBinder::bindSceneTextures(MTL::RenderCommandEncoder* encoder,
        Texture* envAtlas, Texture* shadow, Texture* skyboxCubeMap,
        Texture* reflection, Texture* reflectionDepth, Texture* ssao,
        Texture* areaLightLut1, Texture* areaLightLut2, Texture* sceneColor,
        Texture* reflectionProbeCube, Texture* sceneDepthGrab)
    {
        bindCached(encoder, 2, envAtlas);
        bindCached(encoder, 6, shadow);
        // Slot 7 is NOT a scene slot: it is the material's clearcoat intensity map, bound
        // by bindMaterialTextures before this runs. The scene depth that used to be bound
        // here, which no forward shader reads (particles and SSR take the depth grab at
        // 25), overwrote that map on every draw, and the shader skipped the unbound map.
        bindCached(encoder, 8, skyboxCubeMap);
        bindCached(encoder, 9, reflection);
        bindCached(encoder, 10, reflectionDepth);
        bindCached(encoder, 18, ssao);
        bindCached(encoder, 20, areaLightLut1);
        bindCached(encoder, 21, areaLightLut2);
        bindCached(encoder, 22, sceneColor);
        bindCached(encoder, 24, reflectionProbeCube);
        bindCached(encoder, 25, sceneDepthGrab);
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
    // Light cookies
    // -----------------------------------------------------------------------

    void MetalTextureBinder::bindCookieTextures(MTL::RenderCommandEncoder* encoder,
        Texture* cookie2D0, Texture* cookie2D1, Texture* cookieCube0, Texture* cookieCube1)
    {
        bindCached(encoder, 27, cookie2D0);
        bindCached(encoder, 28, cookie2D1);
        bindCached(encoder, 29, cookieCube0);
        bindCached(encoder, 30, cookieCube1);
    }

    // -----------------------------------------------------------------------
    // Pass lifecycle
    // -----------------------------------------------------------------------

    void MetalTextureBinder::resetPassState()
    {
        _boundTextures.fill(nullptr);
        _dirty = true;
        _boundSamplers.fill(nullptr);
    }
}
