// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektayers on 11.09.2025.
//
#include "shadowRenderer.h"

#include "lightCamera.h"
#include "renderer.h"

namespace visutwin::canvas
{
    bool ShadowRenderer::needsShadowRendering(Light* light) {
        bool needs = light->enabled() && light->castShadows()
            && light->shadowUpdateMode() != ShadowUpdateType::SHADOWUPDATE_NONE && light->visibleThisFrame();

        if (light->shadowUpdateMode() == ShadowUpdateType::SHADOWUPDATE_THISFRAME) {
            light->setShadowUpdateMode(ShadowUpdateType::SHADOWUPDATE_NONE);
        }

        if (needs) {
            _renderer->_shadowMapUpdates += light->numShadowFaces();
        }

        return needs;
    }

    Camera* ShadowRenderer::prepareFace(Light* light, Camera* camera, int face) {
        LightType type = light->type();
        LightRenderData* lightRenderData = getLightRenderData(light, camera, face);
        Camera* shadowCam = lightRenderData->shadowCamera;

        // Assign a render target for the face
        int renderTargetIndex = type == LightType::LIGHTTYPE_DIRECTIONAL ? 0 : face;
        shadowCam->setRenderTarget(light->shadowMap()->renderTargets()[renderTargetIndex]);

        return shadowCam;
    }

    void ShadowRenderer::setupRenderPass(RenderPass* renderPass, Camera* shadowCamera, bool clearRenderTarget)
    {
        std::shared_ptr<RenderTarget> rt = shadowCamera->renderTarget();
        renderPass->init(rt);

        renderPass->depthStencilOps()->clearDepthValue = 1.0f;
        renderPass->depthStencilOps()->clearDepth = clearRenderTarget;

        // If rendering to depth buffer
        if (rt->depthBuffer()) {
            renderPass->depthStencilOps()->storeDepth = true;
        } else {
            // Rendering to color buffer
            renderPass->colorOps()->clearValue = shadowCamera->clearColor();
            renderPass->colorOps()->clear = clearRenderTarget;
            renderPass->depthStencilOps()->storeDepth = false;
        }

        // Not sampling dynamically generated cubemaps
        renderPass->setRequiresCubemaps(false);
    }

    LightRenderData* ShadowRenderer::getLightRenderData(Light* light, Camera* camera, int face)
    {
        return light->getRenderData(light->type() == LightType::LIGHTTYPE_DIRECTIONAL ? camera : nullptr, face);
    }

    Camera* ShadowRenderer::createShadowCamera(ShadowType shadowType, LightType type, int face)
    {
        Camera* shadowCam = LightCamera::create("ShadowCamera", type, face);

        const ShadowTypeInfo* shadowInfo = &shadowTypeInfo.at(shadowType);
        assert(shadowInfo != nullptr);
        const bool isVsm = shadowInfo ? shadowInfo->vsm : false;
        const bool isPcf = shadowInfo ? shadowInfo->pcf : false;

        // Don't clear the color buffer if rendering a depth map
        if (isVsm) {
            shadowCam->setClearColor(Color(0, 0, 0, 0));
        } else {
            shadowCam->setClearColor(Color(1, 1, 1, 1));
        }

        shadowCam->setClearDepthBuffer(true);
        shadowCam->setClearStencilBuffer(false);

        // Clear color buffer only when using it
        shadowCam->setClearColorBuffer(!isPcf);

        return shadowCam;
    }
}
