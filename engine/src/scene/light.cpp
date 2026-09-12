// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 01.10.2025.
//

#include "light.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "graphNode.h"
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
        destroyShadowMap();
    }

    void Light::setShadowResolution(int value)
    {
        // DEVIATION: upstream also clamps to the device's maxTextureSize (or
        // maxCubeMapSize for an omni). This port's GraphicsDevice publishes
        // neither, and the Light a LightComponent owns is built with a null
        // device anyway, so the allocation in ShadowMap::create is where an
        // oversized request would have to be caught.
        value = std::max(value, 1);
        if (_shadowResolution == value) {
            return;
        }
        _shadowResolution = value;

        // Everything else about the shadow already follows the new value: the
        // cascade viewport is recomputed from shadowResolution() during each
        // cull, and the shader's PCF texel size is uploaded per frame. Only the
        // texture is allocated once and only when null, so without this the map
        // keeps its old size and the two silently disagree — the cascade renders
        // into a fraction of the texture it is then sampled across.
        //
        // The render data is NOT cleared, unlike setShadowType: the shadow
        // camera caches no size-dependent state (prepareFace re-points it at the
        // new render target, and the viewport is refreshed by the cull), and an
        // omni's per-face caster classification for this frame would be thrown
        // away with it.
        destroyShadowMap();
    }

    void Light::setShadowType(const ShadowType value)
    {
        if (_shadowType == value) {
            return;
        }
        _shadowType = value;

        // Shadow cameras cache type-dependent state (e.g. clear color: PCF
        // clears to 1.0 depth, VSM to the (0,0,0,0) "unrendered" sentinel) and
        // the shadow map format differs (depth vs RGBA16F moments) — rebuild both.
        _renderData.clear();
        destroyShadowMap();
    }

    void Light::destroyShadowMap()
    {
        _shadowMap = nullptr;

        // A light whose shadow was already rendered once would never render into
        // the replacement, leaving it blank for as long as the light lives.
        if (_shadowUpdateMode == ShadowUpdateType::SHADOWUPDATE_NONE) {
            _shadowUpdateMode = ShadowUpdateType::SHADOWUPDATE_THISFRAME;
        }
    }

    void Light::invalidateRenderData(const Camera* camera)
    {
        std::erase_if(_renderData, [camera](const std::unique_ptr<LightRenderData>& rd) {
            return rd && rd->camera == camera;
        });
    }

    BoundingSphere Light::boundingSphere() const
    {
        if (!_node) {
            return BoundingSphere(Vector3(0.0f), 0.0f);
        }
        // The world matrix's translation column, rather than GraphNode::position(),
        // which is not const. Same value, and the cone branch below needs the matrix
        // anyway for the light's axis.
        const auto& world = _node->worldTransform();
        const Vector3 position(world.getColumn(3));
        if (_type != LightType::LIGHTTYPE_SPOT) {
            return BoundingSphere(position, _range);
        }

        // Upstream's cone bound (light.js getBoundingSphere, after Bart Wronski's
        // "cull that cone"). A spot's range SPHERE is a poor bound for anything but
        // a very wide cone: at 20 degrees the cone occupies about 3% of it, so
        // bounding by the sphere leaves a narrow spot lighting — and re-rendering
        // its shadow map — from most of the places it cannot reach.
        //
        // Two regimes. Past 45 degrees the tight bound is the sphere through the
        // cone's rim, centred on the axis at range*cos; at or under it, the sphere
        // that passes through the apex and the rim, which has radius
        // range / (2 cos) and is centred that far along the axis.
        //
        // The light shines along its node's NEGATIVE Y (LightComponent::direction),
        // which is upstream's convention too — upstream spells the same arithmetic
        // with the node's up vector and a negated scale.
        Vector3 axis = Vector3(world.getColumn(1)) * -1.0f;
        if (axis.lengthSquared() < 1e-8f) {
            axis = Vector3(0.0f, -1.0f, 0.0f);
        } else {
            axis = axis.normalized();
        }

        const float outerRadians = _outerConeAngle * (std::numbers::pi_v<float> / 180.0f);
        const float cosOuter = std::cos(outerRadians);
        if (_outerConeAngle > 45.0f) {
            const float radius = _range * std::sin(outerRadians);
            return BoundingSphere(position + axis * (_range * cosOuter), radius);
        }
        // cosOuter >= cos(45 deg) here, so the division is safe.
        const float radius = _range / (2.0f * cosOuter);
        return BoundingSphere(position + axis * radius, radius);
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
