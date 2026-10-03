// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
//
#include "renderPassPrepass.h"

#include <algorithm>
#include <climits>
#include <utility>

#include "framework/components/camera/cameraComponent.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "scene/camera.h"
#include "scene/composition/layerComposition.h"
#include "scene/composition/renderAction.h"
#include "scene/constants.h"
#include "scene/graphNode.h"
#include "scene/layer.h"
#include "scene/materials/material.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/renderer/depthOnlyDraw.h"
#include "scene/renderer/renderer.h"
#include "scene/shader-lib/programLibrary.h"

#include <spdlog/spdlog.h>

namespace visutwin::canvas
{
    namespace
    {
        /**
         * Whether this mesh instance contributes to the depth the prepass produces: its
         * material WRITES depth, which is exactly what the scene pass will write for it.
         * A blended material does not by default (setAlphaMode turns the write off) but
         * may ask to; an opaque one may turn it off. An alpha-tested (MASK) material does
         * write depth, and its cut-out holes come from the opacity frontend the shadow
         * shader runs — which is why the draw binds such a material.
         *
         * Deliberately NOT the shadow-caster test: a mesh with castShadow off still
         * occludes in screen space.
         */
        bool writesPrepassDepth(MeshInstance* meshInstance)
        {
            if (!meshInstance || !meshInstance->visible() || !meshInstance->mesh()) {
                return false;
            }
            if (meshInstance->node() && !meshInstance->node()->enabled()) {
                return false;
            }
            if (!meshInstance->mesh()->getVertexBuffer()) {
                return false;
            }
            const Material* material = meshInstance->material();
            if (!material) {
                return false;
            }
            const auto& depthState = material->depthState();
            return !depthState || depthState->depthWrite();
        }

        /// The composition slot of a sublayer, or -1 when the composition lacks it.
        int sublayerSlot(const LayerComposition& composition, const int layerId, const bool transparent)
        {
            const auto layer = composition.getLayerById(layerId);
            if (!layer) {
                return -1;
            }
            return transparent ? composition.getTransparentIndex(layer) : composition.getOpaqueIndex(layer);
        }

        /// The viewport size the forward pass gives this camera on a target of the given
        /// size (its normalized rect in whole pixels, at least one), which is what the
        /// jitter offset is scaled by.
        std::pair<int, int> cameraViewportSize(const Camera& camera, const int targetWidth, const int targetHeight)
        {
            const auto clamp01 = [](const float v) { return std::clamp(v, 0.0f, 1.0f); };
            const Vector4& rect = camera.rect();
            const int x = std::clamp(static_cast<int>(clamp01(rect.getX()) * static_cast<float>(targetWidth)),
                0, std::max(targetWidth - 1, 0));
            const float topNorm = clamp01(clamp01(rect.getY()) + clamp01(rect.getW()));
            const int y = std::clamp(targetHeight - static_cast<int>(topNorm * static_cast<float>(targetHeight)),
                0, std::max(targetHeight - 1, 0));
            const int w = std::clamp(std::max(1, static_cast<int>(clamp01(rect.getZ()) * static_cast<float>(targetWidth))),
                1, std::max(targetWidth - x, 1));
            const int h = std::clamp(std::max(1, static_cast<int>(clamp01(rect.getW()) * static_cast<float>(targetHeight))),
                1, std::max(targetHeight - y, 1));
            return {w, h};
        }
    }

    RenderPassPrepass::RenderPassPrepass(const std::shared_ptr<GraphicsDevice>& device, Scene* scene, Renderer* renderer,
        CameraComponent* cameraComponent, Texture* sceneDepthTexture, const std::shared_ptr<RenderPassOptions>& options,
        const std::vector<RenderAction*>& actions, LayerComposition* composition)
        : RenderPass(device), _scene(scene), _renderer(renderer), _cameraComponent(cameraComponent),
          _sceneDepthTexture(sceneDepthTexture), _actions(actions), _composition(composition)
    {
        _name = "RenderPassPrepass";
        _requiresCubemaps = false;
        (void)_scene;
        setOptions(options);
    }

    void RenderPassPrepass::prepareShaders()
    {
        prepareDepthOnlyShaders(device());
    }

    void RenderPassPrepass::execute()
    {
        const auto gd = device();
        if (!gd || !_cameraComponent || !_renderer) {
            return;
        }
        auto* camera = _cameraComponent->camera();
        if (!camera || !camera->node()) {
            return;
        }

        const auto programLibrary = getProgramLibrary(gd);
        if (!programLibrary) {
            return;
        }

        // The prepass writes the same depth the scene pass is about to write, from the
        // same camera, using the depth-only (shadow) programs — this port has no
        // separate SHADER_PREPASS. The scene pass then clears and re-renders that
        // depth for real; what the prepass exists for is the window BETWEEN them, in
        // which lighting-mode SSAO has a populated depth buffer to read. Without it
        // that pass has to run after the scene, and the forward shaders sample an
        // occlusion texture that describes the PREVIOUS frame. Under MSAA it is also
        // the only depth anything after the scene can sample, TAA's reprojection
        // included.
        DepthOnlyShaders shaders;
        shaders.plain = programLibrary->getShadowShader(nullptr, false);
        shaders.dynamicBatch = programLibrary->getShadowShader(nullptr, true);
        if (!shaders.plain) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                spdlog::warn("No depth-only shader for this device — the scene depth "
                    "prepass is disabled, and lighting-mode SSAO with it");
            }
            return;
        }

        // This pass bypasses materials for the common caster, so it owns the state the
        // forward pass would otherwise set per material. No depth bias: unlike a shadow
        // map this depth is read from the same viewpoint that wrote it.
        gd->setMaterial(nullptr);
        gd->setShader(shaders.plain);
        static auto prepassBlendState = std::make_shared<BlendState>();
        static auto prepassDepthState = std::make_shared<DepthState>();
        gd->setBlendState(prepassBlendState);
        gd->setDepthState(prepassDepthState);
        gd->setDepthBias(0.0f, 0.0f, 0.0f);

        // The projection carries the frame's TAA jitter, exactly as the forward pass's
        // does: the depth this writes is what TAA reprojects from, and an unjittered
        // depth sits up to half a pixel off the jittered colour it is paired with.
        Matrix4 projection = camera->projectionMatrix();
        if (camera->jitter() > 0.0f) {
            const RenderTarget* target = renderTarget().get();
            const int targetWidth = std::max(target ? target->width() : gd->size().first, 1);
            const int targetHeight = std::max(target ? target->height() : gd->size().second, 1);
            const auto [viewportWidth, viewportHeight] = cameraViewportSize(*camera, targetWidth, targetHeight);
            const auto [jitterX, jitterY] = camera->jitterOffset(gd->renderVersion(), viewportWidth, viewportHeight);
            projection.setElement(2, 0, projection.getElement(2, 0) + jitterX);
            projection.setElement(2, 1, projection.getElement(2, 1) + jitterY);
        }
        const Matrix4 viewProjection = projection * camera->node()->worldTransform().inverse();

        // Only the sublayers that draw BEFORE the depth layer: what follows it (the
        // transparent half of the world, the UI, the gizmos) is not part of the scene
        // depth the post effects read. A composition without a depth layer draws all
        // of the camera's sublayers. The mesh instances come from the frame's cull of
        // each (camera, layer) pair, the same sets the scene pass draws, batches and the
        // layer's own instances included.
        int depthSlot = INT_MAX;
        if (_composition) {
            const int opaqueSlot = sublayerSlot(*_composition, LAYERID_DEPTH, false);
            const int transparentSlot = sublayerSlot(*_composition, LAYERID_DEPTH, true);
            if (opaqueSlot >= 0) {
                depthSlot = opaqueSlot;
            }
            if (transparentSlot >= 0) {
                depthSlot = std::min(depthSlot, transparentSlot);
            }
        }

        for (const RenderAction* action : _actions) {
            if (!action || !action->layer || action->camera != _cameraComponent ||
                action->layer->id() == LAYERID_DEPTH) {
                continue;
            }
            if (_composition) {
                const int slot = sublayerSlot(*_composition, action->layer->id(), action->transparent);
                if (slot < 0 || slot >= depthSlot) {
                    continue;
                }
            }
            const auto& culled = _renderer->culledInstances(camera, camera->node(), action->layer);
            const auto& meshInstances = action->transparent ? culled.transparent : culled.opaque;
            for (auto* meshInstance : meshInstances) {
                if (writesPrepassDepth(meshInstance)) {
                    drawDepthOnly(gd.get(), programLibrary.get(), meshInstance, viewProjection, shaders);
                }
            }
        }
    }

    void RenderPassPrepass::after()
    {
        RenderPass::after();
        if (const auto gd = device(); gd && _sceneDepthTexture) {
            gd->setSceneDepthMap(_sceneDepthTexture);
        }
    }
}
