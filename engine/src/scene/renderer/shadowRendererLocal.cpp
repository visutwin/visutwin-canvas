// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#include "shadowRendererLocal.h"

#include <cmath>
#include <numbers>

#include "renderPassShadowLocalNonClustered.h"
#include "scene/graphNode.h"
#include "platform/graphics/graphicsDevice.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    Camera* ShadowRendererLocal::prepareLights(std::vector<Light*>& shadowLights, const std::vector<Light*>& lights) {
        Camera* shadowCamera = nullptr;

        for (auto* light : lights) {
            if (_shadowRenderer->needsShadowRendering(light) && light->atlasViewportAllocated()) {
                shadowLights.push_back(light);

                for (int face = 0; face < light->numShadowFaces(); face++) {
                    shadowCamera = _shadowRenderer->prepareFace(light, nullptr, face);
                }
            }
        }

        return shadowCamera;
    }

    void ShadowRendererLocal::buildNonClusteredRenderPasses(FrameGraph* frameGraph, const std::vector<Light*>& localLights)
    {
        for (size_t i = 0; i < localLights.size(); i++) {
            Light* light = localLights[i];
            const bool needsRendering = _shadowRenderer->needsShadowRendering(light);
            if (needsRendering) {
                // Only spotlights support VSM
                bool applyVsm = light->type() == LightType::LIGHTTYPE_SPOT;

                // Omni lights render all 6 cubemap faces; spot lights render 1 face.
                // Omni uses a cubemap depth texture where each face targets a different slice,
                // so rendering all 6 faces writes to separate storage and does not overwrite.
                const int faceCount = light->numShadowFaces();
                for (int face = 0; face < faceCount; face++) {
                    auto renderPass = std::make_shared<RenderPassShadowLocalNonClustered>(_device, _shadowRenderer, light, face, applyVsm);
                    frameGraph->addRenderPass(renderPass);
                }
            }
        }
    }

    void ShadowRendererLocal::cullLocalLights(const std::vector<Light*>& localLights,
        const std::shared_ptr<GraphicsDevice>& device,
        std::vector<std::unique_ptr<ShadowMap>>& ownedShadowMaps)
    {
        // Cache the device pointer for use in buildNonClusteredRenderPasses().
        // The device is needed to create render pass encoders and draw commands.
        _device = device;

        for (auto* light : localLights) {
            if (!light || !light->castShadows() || !light->enabled()) {
                continue;
            }

            // Allocate shadow map if not yet created.
            if (!light->shadowMap()) {
                auto shadowMap = ShadowMap::create(device.get(), light);
                if (shadowMap) {
                    light->setShadowMap(shadowMap.get());
                    ownedShadowMaps.push_back(std::move(shadowMap));
                } else {
                    spdlog::warn("[LocalShadow] shadow map allocation FAILED for light");
                }
            }

            if (!light->shadowMap()) {
                continue;
            }

            const bool isOmni = (light->type() == LightType::LIGHTTYPE_OMNI);

            // Omni lights set up all 6 cubemap faces; spot lights set up 1 face.
            // For omni, LightCamera::create already sets per-face rotation (±X, ±Y, ±Z)
            // and 90° FOV for each face index.
            const int faceCount = light->numShadowFaces();
            for (int face = 0; face < faceCount; ++face) {
                LightRenderData* rd = light->getRenderData(nullptr, face);
                if (!rd || !rd->shadowCamera || !rd->shadowCamera->node()) {
                    continue;
                }

                Camera* shadowCam = rd->shadowCamera.get();
                GraphNode* lightNode = light->node();
                if (!lightNode) {
                    continue;
                }

                // Position shadow camera at the light's world position.
                shadowCam->node()->setPosition(lightNode->position());

                if (light->type() == LightType::LIGHTTYPE_SPOT) {
                    // Spot: orient camera along the light's direction, FOV = outerConeAngle * 2.
                    shadowCam->node()->setRotation(lightNode->rotation());
                    shadowCam->setFov(std::min(light->outerConeAngle() * 2.0f, 179.0f));
                    shadowCam->setNearClip(0.01f);
                    shadowCam->setFarClip(std::max(light->range(), 0.1f));
                } else {
                    // Point (omni): LightCamera::create already sets per-face rotation and 90° FOV.
                    shadowCam->setNearClip(0.01f);
                    shadowCam->setFarClip(std::max(light->range(), 0.1f));
                }

                // Assign the render target for this face.
                int renderTargetIndex = (light->type() == LightType::LIGHTTYPE_DIRECTIONAL) ? 0 : face;
                const auto& rts = light->shadowMap()->renderTargets();
                if (renderTargetIndex < static_cast<int>(rts.size())) {
                    shadowCam->setRenderTarget(rts[renderTargetIndex]);
                }
            }

            // Compute and store the shadow VP matrix.
            // Spot lights: VP matrix used for 2D shadow projection in the fragment shader.
            // Omni lights: VP matrix is not used (cubemap sampling uses direction-based lookup),
            // but we still compute face 0's VP for backward compat / debug.
            if (!isOmni) {
                LightRenderData* rd = light->getRenderData(nullptr, 0);
                if (rd && rd->shadowCamera && rd->shadowCamera->node()) {
                    const Matrix4 shadowVP = rd->shadowCamera->projectionMatrix()
                        * rd->shadowCamera->node()->worldTransform().inverse();

                    // Apply NDC-to-UV viewport bias matrix, matching directional shadow
                    // construction (shadowRendererDirectional.cpp).
                    // Maps NDC x,y [-1,1] → UV [0,1] and z [-1,1] → [0,1] to match
                    // shadow vertex shader's clip.z = 0.5*(clip.z+clip.w) depth mapping.
                    // Metal Y flip: negative Y scale (texture coordinates are top-left origin).
                    Matrix4 viewportBias = Matrix4::identity();
                    viewportBias.setElement(0, 0, 0.5f);        // X: scale by 0.5
                    viewportBias.setElement(3, 0, 0.5f);        // X: translate by 0.5
                    viewportBias.setElement(1, 1, -0.5f);       // Y: scale by -0.5 (Metal Y flip)
                    viewportBias.setElement(3, 1, 0.5f);        // Y: translate by 0.5
                    viewportBias.setElement(2, 2, 0.5f);        // Z: scale by 0.5
                    viewportBias.setElement(3, 2, 0.5f);        // Z: bias by 0.5

                    light->setShadowViewProjection(viewportBias * shadowVP);
                }
            }
        }
    }
}
