// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// Per-pass texture binding deduplication.
// Extracted from MetalGraphicsDevice for single-responsibility decomposition.
//
#pragma once

#include <array>
#include <vector>
#include <Metal/Metal.hpp>

#include "platform/graphics/shaderBindings.h"

namespace visutwin::canvas
{
    class MetalSamplerCache;
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
        // Fragment sampler slots. Slot 0 is the draw's general sampler: the post sampler in
        // a quad pass, the default material sampler otherwise. Slots 1-6 carry the samplers
        // of the material maps whose Texture state is honoured, one map each, in the order
        // of kMaterialSamplerTextureSlots; the forward and shadow chunks declare them as
        // sampler(1) .. sampler(6) under the same names (forward-fragment-head.metal).
        //
        // Sampler budget of the forward fragment function, against Metal's 16 per stage:
        // 7 argument-table samplers (0-6), and at most 14 constexpr sampler DECLARATIONS
        // in the chunks if every one were reachable in one variant (shadow compare x4 —
        // PCF3, PCF1, PCF5 and the omni cube — VSM, environment atlas, reflection probe,
        // grab pass, cookie, clustered cookie, SSR depth, SSR colour, LTC LUT, PCSS raw),
        // which reduce to 4 distinct sampler states. The shadow fragment
        // function declares 2 argument samplers (0 for the opacity map, 1 for the base
        // colour) and no constexpr one.
        static constexpr int kMaterialSamplerCount = 6;
        static constexpr int kFirstMaterialSamplerSlot = 1;
        // A quad draw's Slang program gives each input a sampler slot of its own, since Metal
        // rejects two combined samplers at one index (bindings.slang, written by
        // tools/generate_shader_bindings.py, whose QUAD_* constants these mirror): input n
        // reads the post sampler at kQuadLinearSamplerBase + n and, as a point or depth input,
        // the nearest sampler at kQuadPointSamplerBase + n.
        static constexpr int kQuadLinearSamplerBase = 3;
        static constexpr int kQuadInputCount = 8;
        static constexpr int kQuadPointSamplerBase = 11;
        static constexpr int kQuadPointInputs = 5;
        static constexpr int kSamplerSlotCount = 16;
        static_assert(kFirstMaterialSamplerSlot + kMaterialSamplerCount <= kSamplerSlotCount);
        static_assert(kQuadLinearSamplerBase + kQuadInputCount <= kQuadPointSamplerBase);
        static_assert(kQuadPointSamplerBase + kQuadPointInputs == kSamplerSlotCount);
        // The material TEXTURE slot each material sampler slot follows: base colour,
        // normal, metal-rough / spec-gloss, occlusion, emissive and lightmap — the maps
        // Vulkan reads through a combined image sampler, i.e. through the texture's own
        // sampler. The rest (clearcoat, height, detail normal, gloss, thickness,
        // refraction, opacity) are separate images on Vulkan, read through one shared
        // sampler in the default state, so on Metal they keep the default sampler.
        // Both from the binding table (platform/graphics/shaderBindings.h), which also
        // names every slot: the rows with ownSampler, and the highest fragment slot + 1.
        static constexpr std::array<int, kMaterialSamplerCount> kMaterialSamplerTextureSlots =
            visutwin::canvas::kMaterialSamplerTextureSlots;
        static_assert(visutwin::canvas::kMaterialSamplerTextureSlots.size() == kMaterialSamplerCount);

        static constexpr int kMaxTextureSlots = kMetalMaxTextureSlots;

        /// Bind a texture at the given fragment slot, skipping if already bound.
        void bindCached(MTL::RenderCommandEncoder* encoder, int slot, Texture* texture);

        /// Clear (unbind) a texture slot, skipping if already nullptr.
        void clearCached(MTL::RenderCommandEncoder* encoder, int slot);

        /// Bind a sampler at a fragment sampler slot, skipping if already bound.
        void bindSamplerCached(MTL::RenderCommandEncoder* encoder, int slot, MTL::SamplerState* sampler);

        /// Bind material textures from a pre-queried list of texture slots.
        /// Clears material-owned slots (0,1,3,4,5) that are not present in the list.
        /// Also records, for each material sampler slot, the sampler its texture's own
        /// state asks for (from `samplers`); bindDrawSamplers issues them.
        void bindMaterialTextures(MTL::RenderCommandEncoder* encoder,
            const std::vector<TextureSlot>& textureSlots, MetalSamplerCache& samplers);

        /// Clear the material's slots among 0-7 (used when no material is bound). The
        /// scene slots in that range (2, 6) are bindSceneTextures' to set; 7 is the
        /// clearcoat intensity map, a material slot. The material sampler slots go back
        /// to the default sampler.
        void clearMaterialSlots(MTL::RenderCommandEncoder* encoder);

        /// Issue the draw's samplers: `general` at slot 0 and, unless `materialSamplers`
        /// is false (a quad pass, whose shaders declare slot 0 only), each material
        /// sampler slot's recorded sampler, or `materialDefault` where none is recorded.
        /// Every slot a forward or shadow function declares is bound by the first draw
        /// of a pass, so none is ever left unset.
        void bindDrawSamplers(MTL::RenderCommandEncoder* encoder, MTL::SamplerState* general,
            MTL::SamplerState* materialDefault, bool materialSamplers);

        /// Bind scene-global textures (envAtlas, shadow, sceneDepth, skybox cubemap, reflection, reflectionDepth, ssao).
        void bindSceneTextures(MTL::RenderCommandEncoder* encoder,
            Texture* envAtlas, Texture* shadow, Texture* skyboxCubeMap,
            Texture* reflection = nullptr, Texture* reflectionDepth = nullptr,
            Texture* ssao = nullptr, Texture* areaLightLut1 = nullptr, Texture* areaLightLut2 = nullptr,
            Texture* sceneColor = nullptr, Texture* reflectionProbeCube = nullptr,
            Texture* sceneDepthGrab = nullptr);

        /// Bind quad render textures for all 8 slots.
        void bindQuadTextures(MTL::RenderCommandEncoder* encoder,
            const std::array<Texture*, 8>& quadBindings);

        /// Bind local shadow depth textures at slots 11 and 12 (spot lights, 2D).
        void bindLocalShadowTextures(MTL::RenderCommandEncoder* encoder,
            Texture* shadow0, Texture* shadow1);

        /// Bind omni shadow cubemap depth textures at slots 15 and 16 (point lights, cube).
        void bindOmniShadowTextures(MTL::RenderCommandEncoder* encoder,
            Texture* cube0, Texture* cube1);

        /// Bind light cookies: 2D (spot) at slots 27-28, cubemap (omni) at 29-30.
        void bindCookieTextures(MTL::RenderCommandEncoder* encoder,
            Texture* cookie2D0, Texture* cookie2D1, Texture* cookieCube0, Texture* cookieCube1);

        /// Mark the cache as clean after the first draw in a pass.
        void markClean() { _dirty = false; }

        /// Reset all cached state. Must be called at the start of each render pass.
        void resetPassState();

    private:
        std::array<Texture*, kMaxTextureSlots> _boundTextures{};
        bool _dirty = true;
        // What the encoder holds at each fragment sampler slot (reset with the pass).
        std::array<MTL::SamplerState*, kSamplerSlotCount> _boundSamplers{};
        // What the bound material's maps ask for at each material sampler slot; null is
        // the default sampler. Material state, not encoder state, so it survives a pass
        // boundary and is re-issued by the next pass's first draw.
        std::array<MTL::SamplerState*, kMaterialSamplerCount> _materialSamplers{};
    };
}
