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
        // a quad pass, the default material sampler otherwise. A material's maps are read
        // from the bindless table with the samplers it holds (bindlessTable.h), so no
        // material sampler slot exists; slots 1-2 and 3-15 are the quad draws' (below), 7-10
        // the forward program's scene samplers (MetalGraphicsDevice::bindDrawSampler).
        //
        // Sampler budget of the forward fragment function, against Metal's 16 per stage:
        // 5 argument-table samplers (0, 7-10), and at most 14 constexpr sampler DECLARATIONS
        // in the chunks if every one were reachable in one variant (shadow compare x4 —
        // PCF3, PCF1, PCF5 and the omni cube — VSM, environment atlas, reflection probe,
        // grab pass, cookie, clustered cookie, SSR depth, SSR colour, LTC LUT, PCSS raw),
        // which reduce to 4 distinct sampler states.
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
        static_assert(kQuadLinearSamplerBase + kQuadInputCount <= kQuadPointSamplerBase);
        static_assert(kQuadPointSamplerBase + kQuadPointInputs == kSamplerSlotCount);
        static constexpr int kMaxTextureSlots = kMetalMaxTextureSlots;

        /// Bind a texture at the given fragment slot, skipping if already bound.
        void bindCached(MTL::RenderCommandEncoder* encoder, int slot, Texture* texture);

        /// Clear (unbind) a texture slot, skipping if already nullptr.
        void clearCached(MTL::RenderCommandEncoder* encoder, int slot);

        /// Bind a sampler at a fragment sampler slot, skipping if already bound.
        void bindSamplerCached(MTL::RenderCommandEncoder* encoder, int slot, MTL::SamplerState* sampler);



        /// Issue the draw's samplers: `general` at slot 0 and, unless `materialSamplers`
        /// is false (a quad pass, whose shaders declare slot 0 only), each material
        /// sampler slot's recorded sampler, or `materialDefault` where none is recorded.
        /// Every slot a forward or shadow function declares is bound by the first draw
        /// of a pass, so none is ever left unset.
        void bindDrawSamplers(MTL::RenderCommandEncoder* encoder, MTL::SamplerState* general);

        /// Bind scene-global textures (envAtlas, shadow, sceneDepth, skybox cubemap, reflection, reflectionDepth, ssao).
        void bindSceneTextures(MTL::RenderCommandEncoder* encoder,
            Texture* envAtlas, Texture* shadow, Texture* skyboxCubeMap,
            Texture* reflection = nullptr, Texture* reflectionDepth = nullptr,
            Texture* ssao = nullptr, Texture* areaLightLut1 = nullptr, Texture* areaLightLut2 = nullptr,
            Texture* sceneColor = nullptr, Texture* reflectionProbeCube = nullptr,
            Texture* sceneDepthGrab = nullptr);

        /// Bind a quad draw's inputs: input n at fragment texture n (scene slots 8-11 are rebound
        /// through the cache by the next forward draw).
        void bindQuadTextures(MTL::RenderCommandEncoder* encoder,
            const std::array<Texture*, kQuadTextureInputs>& quadBindings);

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
    };
}
