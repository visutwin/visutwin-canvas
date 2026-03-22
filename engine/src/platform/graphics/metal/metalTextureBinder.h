// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Per-pass texture binding deduplication.
// Extracted from MetalGraphicsDevice for single-responsibility decomposition.
//
#pragma once

#include <array>
#include <vector>
#include <Metal/Metal.hpp>

namespace visutwin::canvas
{
    class Texture;
    struct TextureSlot;

    /**
     * Tracks which textures and samplers are currently bound on a Metal render
     * command encoder and skips redundant setFragmentTexture / setFragmentSamplerState
     * calls.  Instantiated as a member of MetalGraphicsDevice and reset at the
     * start of each render pass.
     */
    class MetalTextureBinder
    {
    public:
        static constexpr int kMaxTextureSlots = 19;  // Slots 0-18; 11-12 = spot shadow, 15-16 = omni shadow cubemaps, 17 = height map, 18 = SSAO

        /// Bind a texture at the given fragment slot, skipping if already bound.
        void bindCached(MTL::RenderCommandEncoder* encoder, int slot, Texture* texture);

        /// Clear (unbind) a texture slot, skipping if already nullptr.
        void clearCached(MTL::RenderCommandEncoder* encoder, int slot);

        /// Bind a sampler at slot 0, skipping if already bound.
        void bindSamplerCached(MTL::RenderCommandEncoder* encoder, MTL::SamplerState* sampler);

        /// Bind material textures from a pre-queried list of texture slots.
        /// Clears material-owned slots (0,1,3,4,5) that are not present in the list.
        void bindMaterialTextures(MTL::RenderCommandEncoder* encoder,
            const std::vector<TextureSlot>& textureSlots);

        /// Clear all 8 texture slots (used when no material is bound).
        void clearAllMaterialSlots(MTL::RenderCommandEncoder* encoder);

        /// Bind scene-global textures (envAtlas, shadow, sceneDepth, skybox cubemap, reflection, reflectionDepth, ssao).
        void bindSceneTextures(MTL::RenderCommandEncoder* encoder,
            Texture* envAtlas, Texture* shadow, Texture* sceneDepth, Texture* skyboxCubeMap,
            Texture* reflection = nullptr, Texture* reflectionDepth = nullptr,
            Texture* ssao = nullptr);

        /// Bind quad render textures for all 8 slots.
        void bindQuadTextures(MTL::RenderCommandEncoder* encoder,
            const std::array<Texture*, 8>& quadBindings);

        /// Bind local shadow depth textures at slots 11 and 12 (spot lights, 2D).
        void bindLocalShadowTextures(MTL::RenderCommandEncoder* encoder,
            Texture* shadow0, Texture* shadow1);

        /// Bind omni shadow cubemap depth textures at slots 15 and 16 (point lights, cube).
        void bindOmniShadowTextures(MTL::RenderCommandEncoder* encoder,
            Texture* cube0, Texture* cube1);

        /// Mark the cache as clean after the first draw in a pass.
        void markClean() { _dirty = false; }

        /// Reset all cached state. Must be called at the start of each render pass.
        void resetPassState();

    private:
        std::array<Texture*, kMaxTextureSlots> _boundTextures{};
        bool _dirty = true;
        MTL::SamplerState* _boundSampler = nullptr;
    };
}
