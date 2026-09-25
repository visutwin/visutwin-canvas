// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis on 01.10.2025.
//
#include "renderPassShadowLocalClustered.h"

#include "depthOnlyDraw.h"
#include "localShadowFace.h"
#include "shadowMap.h"
#include "core/scopedTimer.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "scene/camera.h"
#include "scene/shader-lib/programLibrary.h"

namespace visutwin::canvas
{
    void RenderPassShadowLocalClustered::update(const std::vector<Light*>& localLights)
    {
        _name = "RenderPassShadowLocalClustered";

        // prepare render targets / shadow cameras for rendering
        auto& shadowLights = _shadowRendererLocal->shadowLights();
        shadowLights.clear();
        auto shadowCamera = _shadowRendererLocal->prepareLights(shadowLights, localLights);

        // if any shadows need to be rendered
        const int count = static_cast<int>(shadowLights.size());
        setEnabled(count > 0);
        if (count > 0) {
            // Set up the render pass using any of the cameras — they all target the
            // atlas. NOT cleared: each face clears its own rect in execute().
            _shadowRenderer->setupRenderPass(this, shadowCamera, false);
        }
    }

    void RenderPassShadowLocalClustered::execute()
    {
        if (!_graphicsDevice || !_shadowRendererLocal) {
            return;
        }
        const ScopedMilliseconds shadowTimer(_graphicsDevice->frameCounters().shadowMapTime);
        auto programLibrary = getProgramLibrary(_graphicsDevice);
        if (!programLibrary) {
            return;
        }

        for (Light* light : _shadowRendererLocal->shadowLights()) {
            if (!light || !light->shadowMap() || !light->shadowMap()->shadowTexture()) {
                continue;
            }
            const float atlasSize = static_cast<float>(light->shadowMap()->shadowTexture()->width());
            const int faceCount = light->numShadowFaces();
            for (int face = 0; face < faceCount; ++face) {
                LightRenderData* rd = light->getRenderData(nullptr, face);
                if (!rd || !rd->shadowCamera || !rd->shadowCamera->node()) {
                    continue;
                }
                // The face's rect in pixels: a spot's inset slot, or one of an omni
                // light's six tiles.
                const Vector4 rect = rd->shadowViewport * atlasSize;

                // Clear only this rect, then draw the face into it. The clear rebinds
                // shader and depth state, so the shadow state is bound after it.
                clearDepthRect(_graphicsDevice.get(), rect);
                DepthOnlyShaders shaders;
                if (!bindLocalShadowState(_graphicsDevice.get(), programLibrary.get(), light, shaders)) {
                    return;
                }
                _graphicsDevice->setViewport(rect.getX(), rect.getY(), rect.getZ(), rect.getW());
                _graphicsDevice->setScissor(static_cast<int>(rect.getX()), static_cast<int>(rect.getY()),
                    static_cast<int>(rect.getZ()), static_cast<int>(rect.getW()));
                drawLocalShadowFace(_graphicsDevice.get(), programLibrary.get(), shaders,
                    light, face, rd->shadowCamera.get());
            }
        }
    }
}
