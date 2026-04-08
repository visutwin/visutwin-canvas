// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 01.10.2025.
//

#include "light.h"

#include <algorithm>

#include "renderer/shadowRenderer.h"

namespace visutwin::canvas
{
    LightRenderData::LightRenderData(Camera* camera, int face, Light* light)
        : light(light),
          shadowCamera(ShadowRenderer::createShadowCamera(light->shadowType(), light->type(), face)),
          camera(camera),
          face(face),
          shadowViewport(0, 0, 1, 1),
          shadowScissor(0, 0, 1, 1)
    {
    }

    Light::Light(GraphicsDevice* graphicsDevice, bool clusteredLighting)
        : _device(graphicsDevice), _clusteredLighting(clusteredLighting)
    {

    }

    bool Light::castShadows() const
    {
        return _castShadows && _mask != MaskType::MASK_BAKE && _mask != MaskType::MASK_NONE;
    }

    int Light::numShadowFaces() const
    {
        if (_type == LightType::LIGHTTYPE_DIRECTIONAL) {
            return numCascades();
        }
        if (_type == LightType::LIGHTTYPE_OMNI) {
            return 6;
        }
        return 1;
    }

    int Light::numCascades() const
    {
        return _numCascades;
    }

    //numCascades setter (lines 370-395).
    void Light::setNumCascades(int value)
    {
        value = std::clamp(value, 1, 4);
        if (_numCascades == value) {
            return;
        }
        _numCascades = value;

        // Directional cascades layout:
        //   1 cascade: full texture [(0,0,1,1)]
        //   2 cascades: 2×1 vertical strip [(0,0,0.5,0.5), (0,0.5,0.5,0.5)]
        //   3 cascades: 3 of 4 quadrants
        //   4 cascades: 2×2 grid
        static const std::array<std::array<Vector4, 4>, 4> layouts = {{
            {{ Vector4(0,0,1,1), Vector4(0,0,0,0), Vector4(0,0,0,0), Vector4(0,0,0,0) }},
            {{ Vector4(0,0,0.5f,0.5f), Vector4(0,0.5f,0.5f,0.5f), Vector4(0,0,0,0), Vector4(0,0,0,0) }},
            {{ Vector4(0,0,0.5f,0.5f), Vector4(0,0.5f,0.5f,0.5f), Vector4(0.5f,0,0.5f,0.5f), Vector4(0,0,0,0) }},
            {{ Vector4(0,0,0.5f,0.5f), Vector4(0,0.5f,0.5f,0.5f), Vector4(0.5f,0,0.5f,0.5f), Vector4(0.5f,0.5f,0.5f,0.5f) }}
        }};
        _cascadeViewports = layouts[value - 1];

        // Reset palette and distances
        _shadowMatrixPalette.fill(0.0f);
        _shadowCascadeDistances.fill(0.0f);

        // Destroy existing shadow map to force re-allocation with correct cascade count
        _shadowMap = nullptr;
    }

    LightRenderData* Light::getRenderData(Camera* camera, int face)
    {
        // Return existing
        for (const auto& rd : _renderData) {
            if (rd->camera == camera && rd->face == face) {
                return rd.get();
            }
        }

        // Create new one
        auto rd = std::make_unique<LightRenderData>(camera, face, this);
        auto* rdPtr = rd.get();
        _renderData.push_back(std::move(rd));
        return rdPtr;
    }
}
