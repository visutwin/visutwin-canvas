// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#include "renderPassShadowDirectional.h"

#include <string>
#include <vector>

#include "framework/components/render/renderComponent.h"
#include "framework/batching/batchManager.h"
#include "framework/batching/skinBatchInstance.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "scene/graphNode.h"
#include "scene/morph.h"
#include "scene/shader-lib/programLibrary.h"
#include "shadowRenderer.h"
#include "shadowCasterFiltering.h"
#include "scene/frustumUtils.h"

namespace visutwin::canvas
{
    RenderPassShadowDirectional::RenderPassShadowDirectional(const std::shared_ptr<GraphicsDevice>& device,
        ShadowRenderer* shadowRenderer, Light* light, Camera* camera, Camera* shadowCamera, const int face, const bool allCascadesRendering)
        : RenderPass(device), _shadowRenderer(shadowRenderer), _light(light), _camera(camera),
          _shadowCamera(shadowCamera), _graphicsDevice(device), _face(face), _allCascadesRendering(allCascadesRendering)
    {
        _requiresCubemaps = false;
        _name = "RenderPassShadowDirectional";
        if (_light && _light->node()) {
            _name += "-" + _light->node()->name();
        }
        _name += "-face" + std::to_string(_face);
    }

    void RenderPassShadowDirectional::execute()
    {
        if (!_graphicsDevice || !_light) {
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
        // Skinned/morphed shadow variants are fetched lazily on first use — most
        // scenes have no skinned casters and shouldn't compile the variants.
        std::shared_ptr<Shader> shadowShaderSkinned;
        std::shared_ptr<Shader> shadowShaderSkinnedMorphed;
        std::shared_ptr<Shader> shadowShaderMorphed;

        _graphicsDevice->setShader(shadowShader);

        // Shadow pass needs blend/depth state set on the device — the forward pass
        // sets these per-material, but the shadow pass bypasses materials entirely.
        static auto shadowBlendState = std::make_shared<BlendState>();   // default: no blend, color writes on
        static auto shadowDepthState = std::make_shared<DepthState>();   // default: depth test+write enabled
        _graphicsDevice->setBlendState(shadowBlendState);
        _graphicsDevice->setDepthState(shadowDepthState);

        // hardware polygon-offset depth bias during shadow rendering.
        // bias = shadowBias * -1000.0,
        // applied via device.setDepthState(light.shadowDepthState).
        // The slope-based bias automatically adds more offset on steep geometry, preventing
        // acne without requiring excessive fixed bias that would erase self-shadows.
        {
            // Light::shadowBias() is upstream's NEGATIVE internal value (LightComponent
            // remaps its 0..1 authoring value with -0.01 * clamp), so this product is
            // POSITIVE: it offsets casters away from the light, which is the direction
            // that removes acne.
            //
            // PCSS applies its own bias in the shader (upstream light.js does the same
            // skip); a hardware offset on top of it eats valid contact shadows.
            const float bias = (_light->shadowType() == SHADOW_PCSS_32F)
                ? 0.0f : _light->shadowBias() * -1000.0f;
            _graphicsDevice->setDepthBias(bias, bias, 0.0f);
        }

        // Get shadow map texture dimensions for viewport mapping.
        const int texSize = _light->shadowResolution();

        // Loop over all cascades, rendering each with its own viewport/scissor.
        // loops faces.
        const int faceCount = _light->numShadowFaces();
        for (int face = 0; face < faceCount; ++face) {
            LightRenderData* rd = _light->getRenderData(_camera, face);
            if (!rd || !rd->shadowCamera || !rd->shadowCamera->node()) {
                continue;
            }

            Camera* shadowCam = rd->shadowCamera.get();

            // Map normalized viewport rect to pixel coordinates.
            const Vector4& vpRect = rd->shadowViewport;
            const float vpX = vpRect.getX() * static_cast<float>(texSize);
            const float vpY = vpRect.getY() * static_cast<float>(texSize);
            const float vpW = vpRect.getZ() * static_cast<float>(texSize);
            const float vpH = vpRect.getW() * static_cast<float>(texSize);

            _graphicsDevice->setViewport(vpX, vpY, vpW, vpH);
            _graphicsDevice->setScissor(
                static_cast<int>(vpX), static_cast<int>(vpY),
                static_cast<int>(vpW), static_cast<int>(vpH));

            // Compute this cascade's VP matrix from its shadow camera.
            const Matrix4 viewProjection = shadowCam->projectionMatrix()
                * shadowCam->node()->worldTransform().inverse();

            // Build the cascade's frustum once for the whole caster sweep.
            const Frustum shadowFrustum = (shadowCam && shadowCam->node())
                ? buildCameraFrustum(shadowCam, shadowCam->node()) : Frustum{};

            // Casters: every RenderComponent's mesh instances, PLUS the batch mesh
            // instances, which belong to no RenderComponent (BatchManager registers
            // them straight with the scene layers) and would otherwise cast no shadow.
            std::vector<MeshInstance*> casters;
            for (auto* renderComponent : RenderComponent::instances()) {
                if (!shouldRenderShadowRenderComponent(renderComponent, _camera)) {
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
                    if (!shouldRenderShadowMeshInstance(meshInstance, shadowCam, shadowFrustum)) {
                        continue;
                    }

                    auto vertexBuffer = meshInstance->mesh()->getVertexBuffer();
                    meshInstance->setVisibleThisFrame(true);
                    _graphicsDevice->setVertexBuffer(vertexBuffer, 0);

                    if (meshInstance->isDynamicBatch()) {
                        // Dynamic batch: use dynamic batch shadow shader + palette.
                        if (shadowShaderDynBatch) {
                            _graphicsDevice->setShader(shadowShaderDynBatch);
                        }
                        auto* sbi = meshInstance->skinBatchInstance();
                        if (sbi) {
                            _graphicsDevice->setDynamicBatchPalette(sbi->paletteData(), sbi->paletteSizeBytes());
                        }
                        _graphicsDevice->setTransformUniforms(viewProjection, Matrix4::identity());
                        _graphicsDevice->draw(meshInstance->mesh()->getPrimitive(), meshInstance->mesh()->getIndexBuffer(), 1, -1, true, true);
                        // Restore standard shadow shader for next non-dynamic-batch mesh.
                        _graphicsDevice->setShader(shadowShader);
                    } else if (meshInstance->skinInstance() || meshInstance->morphInstance()) {
                        // Skinned/morphed caster: switch to the matching shadow variant,
                        // bind the bone palette (slot 6) and/or morph buffers (slots 9/10).
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
        }
    }

    void RenderPassShadowDirectional::after()
    {
        // DEVIATION: VSM post-filtering path is not ported yet.
        (void)_face;
    }
}
