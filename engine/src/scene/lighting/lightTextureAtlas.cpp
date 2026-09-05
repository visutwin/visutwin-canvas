// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//

#include "lightTextureAtlas.h"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "scene/light.h"
#include "scene/renderer/shadowMap.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/renderTarget.h"

namespace visutwin::canvas
{
    void LightTextureAtlas::ensureCreated()
    {
        if (_arrayTexture) {
            return;
        }
        _resolution = _pendingResolution;
        _capacity = _pendingCapacity;

        // One Depth texture2d_array: `_capacity` full-resolution slices, one per
        // shadow-casting spot light. Nearest filtering (hardware PCF compares per tap).
        TextureOptions options;
        options.name = "ClusterShadowAtlas";
        options.width = static_cast<uint32_t>(_resolution);
        options.height = static_cast<uint32_t>(_resolution);
        options.format = PixelFormat::PIXELFORMAT_DEPTH;
        options.arrayLength = static_cast<uint32_t>(_capacity);
        options.mipmaps = false;
        options.minFilter = FilterMode::FILTER_NEAREST;
        options.magFilter = FilterMode::FILTER_NEAREST;

        _arrayTexture = std::make_shared<Texture>(_device.get(), options);
        _arrayTexture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        _arrayTexture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);

        // One render target per slice: the depth attachment targets array layer `i`
        // via RenderTargetOptions::face (MetalGraphicsDevice maps face -> slice for
        // depth texture arrays, mirroring the omni cubemap face path).
        _sliceTargets.reserve(static_cast<size_t>(_capacity));
        for (int i = 0; i < _capacity; ++i) {
            RenderTargetOptions rt;
            rt.name = "ClusterShadowSlice";
            rt.depthBuffer = _arrayTexture.get();
            rt.face = i;
            _sliceTargets.push_back(_device->createRenderTarget(rt));
        }
    }

    void LightTextureAtlas::allocate(const std::vector<Light*>& spotShadowLights)
    {
        ensureCreated();

        int slice = 0;
        for (auto* light : spotShadowLights) {
            if (!light) {
                continue;
            }
            if (slice >= _capacity) {
                // Out of atlas slices — this light won't cast a clustered shadow.
                light->setAtlasSlice(-1);
                light->setAtlasViewportAllocated(false);
                continue;
            }
            light->setAtlasSlice(slice);
            light->setAtlasViewportAllocated(true);
            // Wrap this light's ShadowMap around its atlas slice so cullLocalLights +
            // the shadow render pass draw its depth into the array layer.
            light->setShadowMap(ShadowMap::createAtlasSlice(_arrayTexture,
                _sliceTargets[static_cast<size_t>(slice)]));
            ++slice;
        }
    }
    void LightTextureAtlas::configure(const int resolution, const int capacity)
    {
        _pendingResolution = std::max(1, resolution);
        _pendingCapacity = std::max(1, capacity);
    }
}
