// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 06.12.2025.
//
#include "renderPassShadowLocalNonClustered.h"

#include "framework/components/render/renderComponent.h"
#include "framework/batching/skinBatchInstance.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include <scene/graphNode.h>
#include "scene/shader-lib/programLibrary.h"
#include "shadowCasterFiltering.h"

namespace visutwin::canvas
{
    RenderPassShadowLocalNonClustered::RenderPassShadowLocalNonClustered(const std::shared_ptr<GraphicsDevice>& device,
        ShadowRenderer* shadowRenderer, Light* light, int face, bool applyVsm): RenderPass(device),
          _shadowRenderer(shadowRenderer),
          _light(light),
          _graphicsDevice(device),
          _face(face),
          _applyVsm(applyVsm) {

        _requiresCubemaps = false;

        // Prepare the shadow camera for this face
        _shadowCamera = shadowRenderer->prepareFace(light, nullptr, face);

        // Set up the render pass
        // Clear the render target as well, as it contains a single shadow map
        shadowRenderer->setupRenderPass(this, _shadowCamera, true);

        // Set debug name
        if (light->node()) {
            _name = _name + "-" + light->node()->name();
        }
    }

    void RenderPassShadowLocalNonClustered::execute()
    {
        if (!_graphicsDevice || !_shadowCamera || !_shadowCamera->node()) {
            return;
        }

        auto programLibrary = getProgramLibrary(_graphicsDevice);
        if (!programLibrary) {
            return;
        }

        auto shadowShader = programLibrary->getShadowShader(false);
        auto shadowShaderDynBatch = programLibrary->getShadowShader(true);
        if (!shadowShader) {
            return;
        }

        _graphicsDevice->setShader(shadowShader);

        // Shadow pass needs blend/depth state set on the device — the forward pass
        // sets these per-material, but the shadow pass bypasses materials entirely.
        // Matches renderPassShadowDirectional.cpp execute().
        static auto shadowBlendState = std::make_shared<BlendState>();   // default: no blend, color writes on
        static auto shadowDepthState = std::make_shared<DepthState>();   // default: depth test+write enabled
        _graphicsDevice->setBlendState(shadowBlendState);
        _graphicsDevice->setDepthState(shadowDepthState);

        // hardware polygon-offset depth bias during shadow rendering.
        // Applied via setDepthState().
        {
            const float bias = _light->shadowBias() * -1000.0f;
            _graphicsDevice->setDepthBias(bias, bias, 0.0f);
        }

        const Matrix4 viewProjection = _shadowCamera->projectionMatrix() * _shadowCamera->node()->worldTransform().inverse();
        for (auto* renderComponent : RenderComponent::instances()) {
            if (!shouldRenderShadowRenderComponent(renderComponent, nullptr)) {
                continue;
            }

            for (auto* meshInstance : renderComponent->meshInstances()) {
                if (!meshInstance->visible()) {
                    continue;
                }
                if (!shouldRenderShadowMeshInstance(meshInstance, _shadowCamera)) {
                    continue;
                }

                auto vertexBuffer = meshInstance->mesh()->getVertexBuffer();
                meshInstance->setVisibleThisFrame(true);
                _graphicsDevice->setVertexBuffer(vertexBuffer, 0);

                if (meshInstance->isDynamicBatch()) {
                    if (shadowShaderDynBatch) {
                        _graphicsDevice->setShader(shadowShaderDynBatch);
                    }
                    auto* sbi = meshInstance->skinBatchInstance();
                    if (sbi) {
                        _graphicsDevice->setDynamicBatchPalette(sbi->paletteData(), sbi->paletteSizeBytes());
                    }
                    _graphicsDevice->setTransformUniforms(viewProjection, Matrix4::identity());
                    _graphicsDevice->draw(meshInstance->mesh()->getPrimitive(), meshInstance->mesh()->getIndexBuffer(), 1, -1, true, true);
                    _graphicsDevice->setShader(shadowShader);
                } else {
                    const auto modelMatrix = (meshInstance->node() ? meshInstance->node()->worldTransform() : Matrix4::identity());
                    _graphicsDevice->setTransformUniforms(viewProjection, modelMatrix);
                    _graphicsDevice->draw(meshInstance->mesh()->getPrimitive(), meshInstance->mesh()->getIndexBuffer(), 1, -1, true, true);
                }
            }
        }

        (void)_shadowRenderer;
        (void)_light;
        (void)_face;
        (void)_applyVsm;
    }
}
