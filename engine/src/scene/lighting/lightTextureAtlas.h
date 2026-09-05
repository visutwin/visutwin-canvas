// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#pragma once

#include <memory>
#include <vector>

#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
    class Light;
    class Texture;
    class RenderTarget;

    /**
     * Shared depth texture2d_array that packs local (spot) shadow maps for clustered
     * lighting. Each shadow-casting spot light renders into its own array slice, and
     * the clustered fragment shader samples that slice. This lifts the fixed
     * main-light-array limit on the number of shadow-casting local lights.
     *
     * DEVIATION: upstream packs a single 2D atlas with per-light viewport sub-rects;
     * this uses a 2D texture ARRAY (one full-resolution slice per light), which maps
     * cleanly onto Metal per-slice render targets and needs no viewport UV remap.
     * Only spot lights are atlased here; omni (cube) shadows use the main-array path.
     */
    class LightTextureAtlas
    {
    public:
        explicit LightTextureAtlas(const std::shared_ptr<GraphicsDevice>& device) : _device(device) {}

        // Assign atlas slices to the given shadow-casting spot lights (in order, up to
        // capacity()). Wraps each assigned light's ShadowMap around its slice so the
        // standard local-shadow cull/render path targets the atlas. Lights beyond
        // capacity get no slice (Light::atlasSlice() == -1) and should be handled by
        // the caller (e.g. left unshadowed).
        void allocate(const std::vector<Light*>& spotShadowLights);

        /// Sets the per-slice resolution and how many slices the atlas holds. The
        /// request is recorded and applied when the array texture is first created,
        /// so it has to arrive BEFORE the first shadow pass; afterwards it is
        /// ignored, because the ShadowMaps already wrapped around the slices cannot
        /// follow a resize. TODO: recreating the atlas live hangs the renderer, so
        /// the live path is deliberately not taken.
        void configure(int resolution, int capacity);

        Texture* shadowArrayTexture() const { return _arrayTexture.get(); }
        int capacity() const { return _capacity; }
        int resolution() const { return _resolution; }

    private:
        void ensureCreated();

        std::shared_ptr<GraphicsDevice> _device;
        std::shared_ptr<Texture> _arrayTexture;
        std::vector<std::shared_ptr<RenderTarget>> _sliceTargets;

        int _capacity = 16;      // number of shadow-casting spot lights the atlas holds
        int _resolution = 1024;  // per-slice shadow map resolution
        // What configure() asked for; applied on the next (re)creation.
        int _pendingCapacity = 16;
        int _pendingResolution = 1024;
    };
}
