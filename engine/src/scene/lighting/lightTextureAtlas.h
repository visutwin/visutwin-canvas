// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#pragma once

#include <memory>
#include <vector>

#include "core/math/vector4.h"
#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
    class Light;
    class Texture;
    class RenderTarget;
    class ShadowMap;

    /**
     * One packed 2D depth atlas holding every clustered local shadow map (upstream
     * `LightTextureAtlas`). The atlas is split into equal square SLOTS — as many as
     * there are lights that need one this frame, rounded up to a square grid, or
     * whatever `LightingParams::atlasSplit` says — and each shadow-casting spot or
     * omni light is assigned one. A spot renders its single face into the whole
     * slot (inset by a few pixels so filtering cannot read the neighbour); an omni
     * light renders its six faces into a 3x2 grid of tiles a third of the slot wide,
     * and the clustered fragment shader picks the tile from the light-to-fragment
     * direction exactly as a hardware cubemap lookup would.
     *
     * Slots are ranked by size and lights by the screen area they cover, and a light
     * keeps the slot it had whenever the size still matches, so a static light's
     * one-shot shadow survives from frame to frame. A light that is handed a
     * DIFFERENT slot is flagged `atlasSlotUpdated` and re-armed for one render.
     *
     * The memory is the atlas, whatever the light count: 2048x2048 of 32-bit depth is
     * 16 MB, where five omni lights as 1024 cubemaps were 120 MB. What the split
     * gives up is per-light resolution — more lights, smaller slots — which is the
     * trade upstream makes.
     *
     * Rects are normalized (x, y, width, height) with the origin at the TOP-LEFT of
     * the texture, the only origin this engine has (see the texture-origin rule in
     * AGENTS.md): a face renders through a viewport in that space and the shader
     * samples it in that space, so no flip sits between them.
     */
    class LightTextureAtlas
    {
    public:
        /// Pixels a cube face is rendered PAST its 90 degrees, so a filter kernel at
        /// the tile edge still lands inside it (upstream `shadowEdgePixels`). The
        /// shader insets its face UV by the same amount.
        static constexpr int kShadowEdgePixels = 3;

        /// A slot's border, in pixels, kept clear of a spot's viewport for the same
        /// reason (upstream's `scissorVec`, sized for a 5-tap filter).
        static constexpr int kSpotEdgePixels = 4;

        explicit LightTextureAtlas(const std::shared_ptr<GraphicsDevice>& device) : _device(device) {}

        /// The atlas resolution and the split (empty = one equal square per light,
        /// `ceil(sqrt(count))` on a side). Both may change at any time: a new
        /// resolution resizes the texture and target in place on the next update(),
        /// bumps the version so every light is re-slotted and re-armed, and costs one
        /// re-render of the one-shot shadows (upstream's allocateShadowAtlas).
        void configure(int resolution, const std::vector<int>& atlasSplit);

        /// Assigns slots to `lights` for this frame — every shadow-casting spot or
        /// omni light the frame will render — and writes each light's atlas viewport,
        /// per-face render viewports and scissors, and its shadow-map wrapper. Lights
        /// left over when the slots run out get no shadow this frame
        /// (`Light::atlasViewportAllocated()` false).
        void update(const std::vector<Light*>& lights);

        Texture* shadowAtlasTexture() const { return _texture.get(); }
        const std::shared_ptr<RenderTarget>& renderTarget() const { return _renderTarget; }
        int resolution() const { return _resolution; }

        /// Bumped whenever the atlas contents are lost or the slots are re-laid-out;
        /// a light whose recorded version differs no longer owns its slot.
        int version() const { return _version; }

        // ── Pure layout helpers, held by tests/lightTextureAtlasTests.cpp ──────────

        /// The slot rects for a split: `split[0]` squares on a side, and for each of
        /// those cells `split[1 + i * n + j]` (when present and > 1) subdivides it
        /// again. Sorted largest first, which is the order lights are assigned in.
        static std::vector<Vector4> subdivide(const std::vector<int>& split);

        /// The equal-square split for `lightCount` lights.
        static std::vector<int> automaticSplit(int lightCount);

        /// The rect an omni light's face `face` (+X, -X, +Y, -Y, +Z, -Z) renders into
        /// within `slot`: a 3x2 grid of tiles a third of the slot wide.
        static Vector4 omniFaceRect(const Vector4& slot, int face);

        /// A spot's viewport within `slot`: the slot inset by `edgePixels` on every
        /// side, `resolution` being the atlas size the inset is measured against.
        static Vector4 spotViewport(const Vector4& slot, int resolution, int edgePixels);

        /// The face an unnormalized light-to-fragment direction falls on, and its
        /// UV within that face — the shader's `getCubemapFaceCoordinates`, mirrored
        /// so a test can hold it against the face cameras' real projection.
        static Vector2 cubemapFaceCoordinates(const Vector3& dir, int& faceIndex);

    private:
        struct Slot
        {
            Vector4 rect;
            float size = 0.0f;
            const Light* light = nullptr;
            bool used = false;
        };

        void ensureCreated();
        void assignSlot(Light* light, int slotIndex, bool reassigned);
        void writeViewports(Light* light) const;

        std::shared_ptr<GraphicsDevice> _device;
        std::shared_ptr<Texture> _texture;
        std::shared_ptr<RenderTarget> _renderTarget;
        std::shared_ptr<ShadowMap> _shadowMap;

        int _resolution = 2048;
        int _pendingResolution = 2048;
        std::vector<int> _pendingSplit;
        std::vector<int> _split;
        std::vector<Slot> _slots;
        int _version = 0;
    };
}
