// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 06.12.2025.
//
#include "renderPassShadowLocalNonClustered.h"

#include <vector>

#include <spdlog/spdlog.h>

#include "framework/components/render/renderComponent.h"
#include "framework/batching/batchManager.h"
#include "framework/batching/skinBatchInstance.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include <scene/graphNode.h>
#include "scene/morph.h"
#include "scene/shader-lib/programLibrary.h"
#include "shadowCasterFiltering.h"
#include "scene/frustumUtils.h"

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
            // Returning here draws NOTHING into the shadow map, which then reads as
            // its cleared 1.0 and lights every fragment: a total, silent loss of
            // shadows that looks like a shading bug rather than a missing shader.
            // A program-registration mismatch did exactly this on Vulkan, so say it
            // out loud once instead of failing quietly.
            static bool warned = false;
            if (!warned) {
                warned = true;
                spdlog::warn("No shadow shader for this device — local-light "
                    "shadows are disabled");
            }
            return;
        }
        // Skinned/morphed shadow variants, fetched lazily on first use.
        std::shared_ptr<Shader> shadowShaderSkinned;
        std::shared_ptr<Shader> shadowShaderSkinnedMorphed;
        std::shared_ptr<Shader> shadowShaderMorphed;
        // Hardware-instanced casters — one variant per instance stride.
        std::shared_ptr<Shader> shadowShaderInstanced;
        std::shared_ptr<Shader> shadowShaderInstancedColor;

        // This pass bypasses materials. Clear any binding left by the previous
        // forward pass (commonly the skybox at the end of the preceding frame)
        // before the backend resolves its vertex-stage and pipeline state.
        _graphicsDevice->setMaterial(nullptr);
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
            // See renderPassShadowDirectional: the internal bias is negative, so this is a
            // positive (acne-removing) polygon offset. Upstream skips the hardware offset for
            // non-clustered omni lights (they store distance, not depth) and for PCSS, which
            // biases in the shader.
            const bool skipHardwareBias = _light->shadowType() == SHADOW_PCSS_32F ||
                _light->type() == LightType::LIGHTTYPE_OMNI;
            const float bias = skipHardwareBias ? 0.0f : _light->shadowBias() * -1000.0f;
            _graphicsDevice->setDepthBias(bias, bias, 0.0f);
        }

        const Matrix4 viewProjection = _shadowCamera->projectionMatrix() * _shadowCamera->node()->worldTransform().inverse();

        // Build the shadow frustum once for the whole caster sweep.
        const Frustum shadowFrustum = (_shadowCamera && _shadowCamera->node())
            ? buildCameraFrustum(_shadowCamera, _shadowCamera->node()) : Frustum{};

        // Casters: every RenderComponent's mesh instances, PLUS the batch mesh
        // instances, which belong to no RenderComponent (BatchManager registers them
        // straight with the scene layers) and would otherwise cast no shadow.
        std::vector<MeshInstance*> casters;
        for (auto* renderComponent : RenderComponent::instances()) {
            if (!shouldRenderShadowRenderComponent(renderComponent, nullptr)) {
                continue;
            }
            for (auto* meshInstance : renderComponent->meshInstances()) {
                casters.push_back(meshInstance);
            }
        }
        for (auto* meshInstance : BatchManager::batchMeshInstances()) {
            casters.push_back(meshInstance);
        }

        {
            for (auto* meshInstance : casters) {
                if (!meshInstance || !meshInstance->visible()) {
                    continue;
                }
                if (!shouldRenderShadowMeshInstance(meshInstance, _shadowCamera, shadowFrustum)) {
                    continue;
                }

                auto vertexBuffer = meshInstance->mesh()->getVertexBuffer();
                meshInstance->setVisibleThisFrame(true);
                _graphicsDevice->setVertexBuffer(vertexBuffer, 0);

                const auto& instancing = meshInstance->instancingData();
                const bool isInstanced = instancing.vertexBuffer && instancing.count > 0;

                if (isInstanced) {
                    // Hardware instancing: transform through the per-instance matrices,
                    // matching the forward pass (see RenderPassShadowDirectional).
                    const bool instanceColor = instancing.vertexBuffer->format() &&
                        instancing.vertexBuffer->format()->hasInstanceColor();
                    auto& variant = instanceColor ? shadowShaderInstancedColor : shadowShaderInstanced;
                    if (!variant) {
                        variant = programLibrary->getShadowShader(false, false, false, true, instanceColor);
                    }
                    if (variant) {
                        _graphicsDevice->setShader(variant);
                    }
                    _graphicsDevice->setVertexBuffer(instancing.vertexBuffer, 5);
                    _graphicsDevice->setTransformUniforms(viewProjection, Matrix4::identity());
                    _graphicsDevice->draw(meshInstance->mesh()->getPrimitive(), meshInstance->mesh()->getIndexBuffer(), instancing.count, -1, true, true);
                    // Unbind — the backends detect instancing by scanning bound slots.
                    _graphicsDevice->setVertexBuffer(nullptr, 5);
                    _graphicsDevice->setShader(shadowShader);
                } else if (meshInstance->isDynamicBatch()) {
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
                } else if (meshInstance->skinInstance() || meshInstance->morphInstance()) {
                    // Skinned/morphed caster: matching shadow variant + palette/morph buffers.
                    const bool skinned = meshInstance->skinInstance() != nullptr;
                    const bool morphed = meshInstance->morphInstance() != nullptr;
                    auto& variant = skinned
                        ? (morphed ? shadowShaderSkinnedMorphed : shadowShaderSkinned)
                        : shadowShaderMorphed;
                    if (!variant) {
                        variant = programLibrary->getShadowShader(false, skinned, morphed);
                    }
                    if (variant) {
                        _graphicsDevice->setShader(variant);
                    }
                    if (skinned) {
                        auto* si = meshInstance->skinInstance();
                        si->updateMatrixPalette(meshInstance->node());
                        _graphicsDevice->setDynamicBatchPalette(si->paletteData(), si->paletteSizeBytes());
                    }
                    if (morphed) {
                        auto* mi = meshInstance->morphInstance();
                        if (mi->morph() && mi->morph()->deltaBuffer()) {
                            const auto& params = mi->gpuParams();
                            _graphicsDevice->setMorphState(mi->morph()->deltaBuffer(), &params, sizeof(params));
                        }
                    }
                    const auto modelMatrix = (meshInstance->node() ? meshInstance->node()->worldTransform() : Matrix4::identity());
                    _graphicsDevice->setTransformUniforms(viewProjection, modelMatrix);
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
