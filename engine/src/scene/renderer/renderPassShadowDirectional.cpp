// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#include "renderPassShadowDirectional.h"

#include <string>
#include <vector>

#include "core/scopedTimer.h"
#include "framework/components/render/renderComponent.h"
#include "framework/batching/skinBatchInstance.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "scene/graphNode.h"
#include "scene/materials/standardMaterial.h"
#include "scene/morph.h"
#include "scene/shader-lib/programLibrary.h"
#include "shadowRenderer.h"
#include "depthOnlyDraw.h"
#include "shadowCasterFiltering.h"
#include "spdlog/spdlog.h"
#include "scene/frustumUtils.h"

namespace visutwin::canvas
{
    RenderPassShadowDirectional::RenderPassShadowDirectional(const std::shared_ptr<GraphicsDevice>& device,
        Light* light, Camera* camera, const int face)
        : RenderPass(device), _light(light), _camera(camera), _graphicsDevice(device), _face(face)
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
        const ScopedMilliseconds shadowTimer(_graphicsDevice->frameCounters().shadowMapTime);

        auto programLibrary = getProgramLibrary(_graphicsDevice);
        if (!programLibrary) {
            return;
        }

        // A VSM light's pass writes EVSM moments; the light decides, not a scene flag.
        const bool vsm = _light->shadowType() == SHADOW_VSM_16F;
        auto shadowShader = programLibrary->getShadowShader(nullptr, false, false, false, false, false, vsm);
        auto shadowShaderDynBatch = programLibrary->getShadowShader(nullptr, true, false, false, false, false, vsm);
        if (!shadowShader) {
            // Returning here draws NOTHING into the shadow map, which then reads as
            // its cleared 1.0 and lights every fragment: a total, silent loss of
            // shadows that looks like a shading bug rather than a missing shader.
            // A program-registration mismatch did exactly this on Vulkan, so say it
            // out loud once instead of failing quietly.
            static bool warned = false;
            if (!warned) {
                warned = true;
                spdlog::warn("No shadow shader for this device — directional "
                    "shadows are disabled");
            }
            return;
        }
        // The remaining variants are fetched lazily on first use by drawDepthOnly —
        // most scenes have no skinned or instanced casters and should not compile them.
        DepthOnlyShaders shaders;
        shaders.plain = shadowShader;
        shaders.dynamicBatch = shadowShaderDynBatch;
        shaders.vsm = vsm;

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

            // The same collector the FIT uses, so the two cannot disagree about what
            // a caster is again. Batch mesh instances belong to no RenderComponent —
            // BatchManager registers them straight with the scene layers — and each
            // of these two sweeps used to decide separately whether to include them.
            std::vector<MeshInstance*> casters;
            collectShadowCasters(casters, _camera);

            {
                for (auto* meshInstance : casters) {
                    if (!meshInstance || !meshInstance->visible()) {
                        continue;
                    }
                    if (!shouldRenderShadowMeshInstance(meshInstance, shadowCam, shadowFrustum)) {
                        continue;
                    }

                    if (drawDepthOnly(_graphicsDevice.get(), programLibrary.get(), meshInstance,
                            viewProjection, shaders)) {
                        _graphicsDevice->frameCounters().shadowDrawCalls++;
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
