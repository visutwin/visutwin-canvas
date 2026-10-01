// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.10.2025.
//
#include "shadowRendererDirectional.h"

#include <array>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <optional>

#include "lightCamera.h"
#include "renderPassShadowDirectional.h"
#include "renderPassVsmBlur.h"
#include "renderer.h"
#include "shadowCasterFiltering.h"
#include "scene/frustumUtils.h"
#include "shadowMap.h"
#include "shadowRenderer.h"
#include "framework/components/render/renderComponent.h"
#include "scene/graphNode.h"
#include "scene/meshInstance.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    ShadowRendererDirectional::ShadowRendererDirectional(const std::shared_ptr<GraphicsDevice>& device,
        ShadowRenderer* shadowRenderer)
        : _shadowRenderer(shadowRenderer), _device(device)
    {
    }

    // lines 204-216.
    void ShadowRendererDirectional::generateSplitDistances(Light* light, const float nearDist, const float farDist)
    {
        float* distances = light->shadowCascadeDistancesData();
        const int numCascades = light->numCascades();
        const float distribution = light->cascadeDistribution();

        // Fill all 4 with farDist as default
        for (int i = 0; i < 4; ++i) {
            distances[i] = farDist;
        }

        for (int i = 1; i < numCascades; ++i) {
            const float fraction = static_cast<float>(i) / static_cast<float>(numCascades);
            const float linearDist = nearDist + (farDist - nearDist) * fraction;
            const float logDist = nearDist * std::pow(farDist / std::max(nearDist, 0.001f), fraction);
            distances[i - 1] = linearDist + (logDist - linearDist) * distribution;
        }
        distances[numCascades - 1] = farDist;
    }

    namespace
    {
        // Shadow camera rotation: align -Z with the light direction. Directional lights
        // emit along -Y, so upstream applies the light rotation then rotates -90° on X
        // to map -Y → -Z.
        Quaternion shadowCameraRotation(const Light& light)
        {
            const Quaternion lightRotation = light.node() ? light.node()->rotation() : Quaternion();
            return lightRotation * Quaternion::fromEulerAngles(-90.0f, 0.0f, 0.0f);
        }

        /// A cascade's shadow camera, when the light has render data for it.
        struct CascadeCamera
        {
            LightRenderData* renderData = nullptr;
            Camera* camera = nullptr;
            GraphNode* node = nullptr;
        };

        std::optional<CascadeCamera> cascadeCamera(ShadowRenderer& shadowRenderer, Light* light, Camera* camera,
            const int cascade)
        {
            LightRenderData* renderData = shadowRenderer.getLightRenderData(light, camera, cascade);
            if (!renderData || !renderData->shadowCamera) {
                return std::nullopt;
            }
            Camera* shadowCam = renderData->shadowCamera.get();
            GraphNode* node = shadowCam->node();
            if (!node) {
                return std::nullopt;
            }
            return CascadeCamera{renderData, shadowCam, node};
        }

        /// The world-space bounding sphere of the camera frustum between two depths:
        /// the corners' centroid, and the farthest corner from it.
        void frustumSliceSphere(Camera& camera, const Matrix4& cameraWorld, const float nearDist,
            const float farDist, Vector3& center, float& radius)
        {
            auto frustumPoints = camera.getFrustumCorners(nearDist, farDist);
            center = Vector3(0.0f);
            for (int i = 0; i < 8; ++i) {
                frustumPoints[i] = cameraWorld.transformPoint(frustumPoints[i]);
                center = center + frustumPoints[i];
            }
            center = center * (1.0f / 8.0f);

            radius = 0.0f;
            for (int i = 0; i < 8; ++i) {
                const float dist = (frustumPoints[i] - center).length();
                if (dist > radius) {
                    radius = dist;
                }
            }
        }

        // Pixel-align the shadow camera position to avoid shadow swimming. Mirrors
        // upstream:
        //   sizeRatio = 0.25 * shadowResolution / radius
        // (algebraically equivalent to 0.5 * cascadeRes / radius for the 4-cascade 2×2
        // atlas layout, since cascadeRes = 0.5·resolution.) Only the lateral position
        // gets quantised, as in upstream shadow-renderer-directional.js; depth is taken
        // straight from the centroid.
        Vector3 snapToShadowTexels(const Vector3& center, const float radius, const int resolution,
            const Matrix4& shadowRotMat)
        {
            if (resolution <= 0 || radius <= 0.0f) {
                return center;
            }
            const float sizeRatio = 0.25f * static_cast<float>(resolution) / radius;
            const Vector3 right = Vector3(shadowRotMat.getColumn(0));
            const Vector3 up = Vector3(shadowRotMat.getColumn(1));
            const Vector3 forward = Vector3(shadowRotMat.getColumn(2));
            const float x = std::ceil(center.dot(up) * sizeRatio) / sizeRatio;
            const float y = std::ceil(center.dot(right) * sizeRatio) / sizeRatio;
            const float z = center.dot(forward);
            return up * x + right * y + forward * z;
        }

        // Position the shadow camera far behind the center, looking along the light,
        // with an orthographic projection that encompasses the cascade's bounding
        // sphere. upstream positions at center + forward * 1,000,000 initially for
        // culling, then tightens near/far to the actual caster depth range (see
        // fitDepthRange).
        void placeWideShadowCamera(const CascadeCamera& cascade, const Quaternion& rotation,
            const Vector3& center, const float radius)
        {
            cascade.node->setRotation(rotation);
            cascade.node->setPosition(center);
            cascade.node->translateLocal(0.0f, 0.0f, 1000000.0f);

            cascade.camera->setProjection(ProjectionType::Orthographic);
            cascade.camera->setOrthoHeight(radius);
            cascade.camera->setNearClip(0.01f);
            cascade.camera->setFarClip(2000000.0f);
            cascade.camera->setAspectRatio(1.0f);
        }

        /// The union of the casters' bounds inside the frustum (a caster that is not
        /// culled always counts); nullopt when none is.
        std::optional<BoundingBox> casterBounds(const std::vector<MeshInstance*>& casters, const Frustum& frustum)
        {
            std::optional<BoundingBox> bounds;
            for (auto* meshInstance : casters) {
                const BoundingBox worldAabb = meshInstance->aabb();
                // The rest of shouldRenderShadowMeshInstance: the frustum, for a
                // caster that is culled at all.
                if (meshInstance->cull() && !isVisibleInFrustum(frustum, worldAabb)) {
                    continue;
                }
                if (!bounds) {
                    bounds = worldAabb;
                } else {
                    bounds->add(worldAabb);
                }
            }
            return bounds;
        }

        // Translate the shadow camera so the near plane sits just behind the nearest
        // point of the box along the light, and set the far clip to the box's depth span.
        void fitDepthRange(const CascadeCamera& cascade, const BoundingBox& fitAabb)
        {
            const Matrix4 shadowCamView = cascade.node->worldTransform().inverse();
            const Vector3 c = fitAabb.center();
            const Vector3 h = fitAabb.halfExtents();
            float depthMin = 1e30f;
            float depthMax = -1e30f;
            for (int i = 0; i < 8; ++i) {
                const Vector3 corner = c + h * Vector3(
                    (i & 1) ? 1.0f : -1.0f,
                    (i & 2) ? 1.0f : -1.0f,
                    (i & 4) ? 1.0f : -1.0f);
                const float z = shadowCamView.transformPoint(corner).getZ();
                if (z < depthMin) depthMin = z;
                if (z > depthMax) depthMax = z;
            }
            cascade.node->translateLocal(0.0f, 0.0f, depthMax + 0.1f);
            cascade.camera->setFarClip(depthMax - depthMin + 0.2f);
        }

        // The casters the pass draws, against the FITTED frustum — the test the pass
        // used to run itself, after collecting the whole scene a second time. Not the
        // set the sweep found: that one is tested from a camera a million units
        // back, where the side planes are only good to about a tenth of a unit, and
        // on a 40k-draw frame the two disagreed about six casters sitting on a
        // cascade's edge. The fitted camera is close, so this is the exact answer,
        // and it costs a frustum test per caster rather than a sweep of the scene.
        void recordFittedCasters(const CascadeCamera& cascade, const std::vector<MeshInstance*>& casters,
            const int frame)
        {
            auto& visibleCasters = cascade.renderData->visibleCasters;
            visibleCasters.clear();
            cascade.renderData->visibleCastersFrame = frame;
            const Frustum fittedFrustum = buildCameraFrustum(cascade.camera, cascade.node);
            for (auto* meshInstance : casters) {
                if (meshInstance->cull() && !isVisibleInFrustum(fittedFrustum, meshInstance->aabb())) {
                    continue;
                }
                visibleCasters.push_back(meshInstance);
            }
        }

        // The viewport-scaled shadow matrix for this cascade, stored in the light's
        // matrix palette (column-major, 16 floats per cascade):
        //   shadowMatrix = viewportMatrix × shadowCamProj × shadowCamView
        void storeCascadeShadowMatrix(Light& light, const int cascade, const CascadeCamera& cascadeCam)
        {
            const Matrix4 shadowView = cascadeCam.node->worldTransform().inverse();
            const Matrix4 shadowVP = cascadeCam.camera->projectionMatrix() * shadowView;

            const Vector4& vp = light.cascadeViewports()[cascade];
            // upstream Mat4.setViewport: maps clip coords to the cascade's viewport
            // sub-region, with the Y scale negated for the top-left texture origin (the
            // translate stays upstream's, since the viewport rect is already top-left) and
            // NDC z [-1,1] -> [0,1] baked in because the shader reads the final shadow depth.
            // LightCamera::viewportProjectionBias is exactly that matrix, the one the
            // clustered spot rects use too.
            const Matrix4 shadowMatrix = LightCamera::viewportProjectionBias(vp) * shadowVP;

            // Matrix4 is 64 bytes on all SIMD backends — memcpy directly (same H1 fix
            // as SkinBatchInstance::updateMatrices).
            float* palette = light.shadowMatrixPaletteData();
            std::memcpy(&palette[cascade * 16], &shadowMatrix, 64);
        }
    }

    void ShadowRendererDirectional::cull(Light* light, Camera* camera)
    {
        if (!light || !camera || !_shadowRenderer || light->type() != LightType::LIGHTTYPE_DIRECTIONAL) {
            return;
        }

        // Compute split distances for all cascades.
        const float nearDist = camera->nearClip();
        const float farDist = std::min(camera->farClip(), light->shadowDistance());
        generateSplitDistances(light, nearDist, farDist);

        const Quaternion shadowRotation = shadowCameraRotation(*light);
        // The shadow camera's axes, for pixel alignment.
        const Matrix4 shadowRotMat = Matrix4::trs(Vector3(0.0f), shadowRotation, Vector3(1.0f));
        const Matrix4 cameraWorldMat = camera->node() ? camera->node()->worldTransform() : Matrix4::identity();

        const int cascadeCount = std::min(light->numCascades(), 4);
        const float* distances = light->shadowCascadeDistances().data();

        // The scene's casters for this camera, collected ONCE for all the cascades, with
        // everything that does not depend on a cascade decided here too: what is left
        // per cascade is the frustum test. Each cascade used to collect and test the
        // whole scene itself, and the pass then did both again.
        //
        // The SAME caster set the pass will draw, through the shared collector: this used
        // to sweep RenderComponent::instances() by hand and so missed the batch mesh
        // instances, which belong to no component; the pass drew them anyway, into a depth
        // range fitted without them. A batch outside that range was clipped out of the
        // shadow map — its shadow simply absent, with nothing to say why, and only in a
        // scene that batches at all.
        static thread_local std::vector<MeshInstance*> casters;
        casters.clear();
        collectShadowCasters(casters, camera);
        std::erase_if(casters, [](MeshInstance* meshInstance) {
            return !meshInstance || !meshInstance->visible() ||
                !shouldRenderShadowMeshInstanceIgnoringVisibility(meshInstance);
        });

        // ── Pass 1: place each cascade's camera and measure its casters ──────────
        // The camera covers the cascade's slice of the view frustum, wide in depth.
        // Its caster box, kept for pass 2, is the union of the casters it sees from
        // there (orthoHeight = radius, farClip = 2e6: everything laterally inside the
        // cascade, at any depth — which also catches casters above the slice, such as
        // wing tips higher than the ground the cascade covers). The caster set is
        // rotation-invariant for static scenes, so the depth range fitted from it is
        // identical frame to frame and stored EVSM moments do not drift — what
        // eliminated the wing-tip flicker the old frustum-corner depth produced.
        std::array<std::optional<BoundingBox>, 4> cascadeAabb{};
        for (int cascade = 0; cascade < cascadeCount; ++cascade) {
            const auto cascadeCam = cascadeCamera(*_shadowRenderer, light, camera, cascade);
            if (!cascadeCam) {
                continue;
            }
            cascadeCam->renderData->shadowViewport = light->cascadeViewports()[cascade];
            cascadeCam->renderData->shadowScissor = light->cascadeViewports()[cascade];

            Vector3 center;
            float radius;
            frustumSliceSphere(*camera, cameraWorldMat, cascade == 0 ? nearDist : distances[cascade - 1],
                distances[cascade], center, radius);
            center = snapToShadowTexels(center, radius, light->shadowResolution(), shadowRotMat);
            placeWideShadowCamera(*cascadeCam, shadowRotation, center, radius);

            cascadeAabb[cascade] = casterBounds(casters, buildCameraFrustum(cascadeCam->camera, cascadeCam->node));
        }

        // ── Pass 2: depth-range tightening, then the casters and the shadow matrix ─
        // Split from the sweep above because PCSS needs the UNION of the cascades'
        // caster boxes, which is not known until all of them have been swept.
        //
        // PCSS scales its penumbra by the cascade's caster DEPTH RANGE, so a range
        // that moves makes the softness move with it: a single mesh crossing a
        // cascade's cull boundary changes that cascade's range and the shadows under
        // it visibly change width. Tightening every cascade against the union instead
        // makes the range depend on the whole visible caster set rather than on which
        // cascade a caster happens to land in, so it stops jumping.
        //
        // Only PCSS. The other shadow types never read the range — it is a fit, not a
        // shader input — and they are better off with per-cascade tightening, which
        // buys them depth precision.
        std::optional<BoundingBox> unionAabb;
        if (light->shadowType() == ShadowType::SHADOW_PCSS_32F) {
            for (int cascade = 0; cascade < cascadeCount; ++cascade) {
                if (!cascadeAabb[cascade]) {
                    continue;
                }
                if (!unionAabb) {
                    unionAabb = cascadeAabb[cascade];
                } else {
                    unionAabb->add(*cascadeAabb[cascade]);
                }
            }
        }

        const int frame = _device ? _device->renderVersion() : -1;
        for (int cascade = 0; cascade < cascadeCount; ++cascade) {
            const auto cascadeCam = cascadeCamera(*_shadowRenderer, light, camera, cascade);
            if (!cascadeCam) {
                continue;
            }
            // The union where PCSS asked for it — note it tightens even a cascade with
            // no casters of its own, which is the point: its range must still be
            // sensible. Otherwise this cascade's own box, and nothing at all when it
            // has none, leaving the wide camera pass 1 set up (whose shadow pass is
            // then a no-op anyway).
            if (const auto& fitAabb = unionAabb ? unionAabb : cascadeAabb[cascade]) {
                fitDepthRange(*cascadeCam, *fitAabb);
            }
            recordFittedCasters(*cascadeCam, casters, frame);
            storeCascadeShadowMatrix(*light, cascade, *cascadeCam);
        }
    }

    std::shared_ptr<RenderPass> ShadowRendererDirectional::getLightRenderPass(Light* light, Camera* camera,
        const int face, const bool clearRenderTarget)
    {
        if (!_shadowRenderer || !_device || !light || !camera || light->type() != LightType::LIGHTTYPE_DIRECTIONAL) {
            return nullptr;
        }

        // Prepare all cascade faces (each gets its render target assigned).
        const int faceCount = light->numShadowFaces();
        Camera* shadowCamera = nullptr;
        for (int f = 0; f < faceCount; ++f) {
            shadowCamera = _shadowRenderer->prepareFace(light, camera, f);
        }
        if (!shadowCamera) {
            return nullptr;
        }

        auto renderPass = std::make_shared<RenderPassShadowDirectional>(_device, light, camera, face);
        _shadowRenderer->setupRenderPass(renderPass.get(), shadowCamera, clearRenderTarget);
        return renderPass;
    }

    void ShadowRendererDirectional::buildNonClusteredRenderPasses(FrameGraph* frameGraph,
        const std::unordered_map<Camera*, std::vector<Light*>>& cameraDirShadowLights)
    {
        if (!frameGraph || !_shadowRenderer || !_device) {
            return;
        }

        for (const auto& [camera, lights] : cameraDirShadowLights) {
            if (!camera) {
                continue;
            }
            for (auto* light : lights) {
                if (!light || light->type() != LightType::LIGHTTYPE_DIRECTIONAL) {
                    continue;
                }
                if (!_shadowRenderer->needsShadowRendering(light)) {
                    continue;
                }

                // Single render pass per light — the pass internally loops over all cascades
                // with per-cascade viewport/scissor.
                auto renderPass = getLightRenderPass(light, camera, 0, true);
                if (renderPass) {
                    frameGraph->addRenderPass(renderPass);
                }

                // EVSM_16F: ping-pong gaussian blur on the moments texture so
                // forward sampling sees a filtered variance and produces stable,
                // soft shadows (without blur, the per-texel variance shimmers
                // as receiver geometry moves through sub-texel positions).
                if (light->shadowType() == SHADOW_VSM_16F && light->shadowMap()) {
                    ShadowMap* sm = light->shadowMap();
                    if (sm->blurTempTexture() && sm->blurTempRenderTarget() &&
                        !sm->renderTargets().empty()) {
                        const int resolution = light->shadowResolution();
                        // Convert upstream-style total-tap count to half-kernel size.
                        // vsmBlurSize is total taps and should be odd; halfSize = (taps - 1) / 2.
                        const int filterSize = std::max(1, (light->vsmBlurSize() - 1) / 2);
                        // Multi-cascade atlases pack 0.5x0.5 quadrants — the
                        // blur must not mix moments across cascade seams.
                        const float tileSize = light->numCascades() > 1 ? 0.5f : 1.0f;
                        // Pass 1 — horizontal: shadowTexture → blurTemp.
                        auto blurH = std::make_shared<RenderPassVsmBlur>(_device,
                            sm->shadowTexture(), sm->blurTempRenderTarget(),
                            resolution, true, filterSize, tileSize);
                        // Pass 2 — vertical: blurTemp → shadowTexture.
                        auto blurV = std::make_shared<RenderPassVsmBlur>(_device,
                            sm->blurTempTexture(), sm->renderTargets()[0],
                            resolution, false, filterSize, tileSize);
                        frameGraph->addRenderPass(blurH);
                        frameGraph->addRenderPass(blurV);
                    }
                }
            }
        }
    }
}
