// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.10.2025.
//
#include "shadowRendererDirectional.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

#include "renderPassShadowDirectional.h"
#include "renderPassVsmBlur.h"
#include "renderer.h"
#include "shadowCasterFiltering.h"
#include "shadowMap.h"
#include "shadowRenderer.h"
#include "framework/components/render/renderComponent.h"
#include "scene/graphNode.h"
#include "scene/meshInstance.h"

namespace visutwin::canvas
{
    ShadowRendererDirectional::ShadowRendererDirectional(const std::shared_ptr<GraphicsDevice>& device,
        Renderer* renderer, ShadowRenderer* shadowRenderer)
        : _renderer(renderer), _shadowRenderer(shadowRenderer), _device(device)
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

    void ShadowRendererDirectional::cull(Light* light, Camera* camera)
    {
        if (!light || !camera || !_shadowRenderer || light->type() != LightType::LIGHTTYPE_DIRECTIONAL) {
            return;
        }

        // lines 72-201.
        // Compute split distances for all cascades.
        const float nearDist = camera->nearClip();
        const float farDist = std::min(camera->farClip(), light->shadowDistance());
        generateSplitDistances(light, nearDist, farDist);

        // Get light direction from the light's node. Directional lights emit along -Y.
        GraphNode* lightNode = light->node();
        Vector3 lightDir(0.0f, -1.0f, 0.0f);
        Quaternion lightRotation;
        if (lightNode) {
            const auto& lightWorld = lightNode->worldTransform();
            lightDir = Vector3(lightWorld.getColumn(1)) * -1.0f;
            if (lightDir.lengthSquared() < 1e-8f) {
                lightDir = Vector3(0.0f, -1.0f, 0.0f);
            } else {
                lightDir = lightDir.normalized();
            }
            lightRotation = lightNode->rotation();
        }

        // Shadow camera rotation: align -Z with lightDir.
        // upstream applies the light rotation then rotates -90° on X to map -Y → -Z.
        const Quaternion pitchDown = Quaternion::fromEulerAngles(-90.0f, 0.0f, 0.0f);
        const Quaternion shadowRotation = lightRotation * pitchDown;

        // Build shadow-camera rotation matrix for pixel alignment calculations.
        const Matrix4 shadowRotMat = Matrix4::trs(Vector3(0.0f), shadowRotation, Vector3(1.0f));

        // Camera world transform for transforming frustum corners to world space.
        const Matrix4 cameraWorldMat = camera->node()
            ? camera->node()->worldTransform() : Matrix4::identity();

        const int numCascades = light->numCascades();
        const float* distances = light->shadowCascadeDistances().data();
        const int resolution = light->shadowResolution();

        for (int cascade = 0; cascade < numCascades; ++cascade) {
            LightRenderData* lightRenderData = _shadowRenderer->getLightRenderData(light, camera, cascade);
            if (!lightRenderData || !lightRenderData->shadowCamera) {
                continue;
            }

            Camera* shadowCam = lightRenderData->shadowCamera.get();
            auto& shadowCamNode = shadowCam->node();
            if (!shadowCamNode) {
                continue;
            }

            // Set cascade viewport/scissor from the light's cascade layout.
            lightRenderData->shadowViewport = light->cascadeViewports()[cascade];
            lightRenderData->shadowScissor = light->cascadeViewports()[cascade];

            // Get frustum corners for this cascade's depth slice.
            const float frustumNear = (cascade == 0) ? nearDist : distances[cascade - 1];
            const float frustumFar = distances[cascade];
            auto frustumPoints = camera->getFrustumCorners(frustumNear, frustumFar);

            // Transform corners to world space and compute bounding sphere center.
            Vector3 center(0.0f);
            for (int i = 0; i < 8; ++i) {
                frustumPoints[i] = cameraWorldMat.transformPoint(frustumPoints[i]);
                center = center + frustumPoints[i];
            }
            center = center * (1.0f / 8.0f);

            // Compute bounding sphere radius (max distance from center to any corner).
            float radius = 0.0f;
            for (int i = 0; i < 8; ++i) {
                const float dist = (frustumPoints[i] - center).length();
                if (dist > radius) {
                    radius = dist;
                }
            }

            // Pixel-align shadow camera position to avoid shadow swimming.
            // Mirrors upstream:
            //   sizeRatio = 0.25 * shadowResolution / radius
            // (algebraically equivalent to 0.5 * cascadeRes / radius for the
            // 4-cascade 2×2 atlas layout, since cascadeRes = 0.5·resolution.)
            if (resolution > 0 && radius > 0.0f) {
                const float sizeRatio = 0.25f * static_cast<float>(resolution) / radius;

                // Extract shadow camera axes from rotation matrix.
                const Vector3 right = Vector3(shadowRotMat.getColumn(0));
                const Vector3 up = Vector3(shadowRotMat.getColumn(1));
                const Vector3 forward = Vector3(shadowRotMat.getColumn(2));

                // Project center onto shadow camera axes, snap right/up to a
                // texel grid, leave forward unsnapped. Mirrors upstream
                // shadow-renderer-directional.js: only the lateral position
                // gets quantised; depth is taken straight from the centroid.
                const float x = std::ceil(center.dot(up) * sizeRatio) / sizeRatio;
                const float y = std::ceil(center.dot(right) * sizeRatio) / sizeRatio;
                const float z = center.dot(forward);

                center = up * x + right * y + forward * z;
            }

            // Position shadow camera far behind the center, looking along lightDir.
            // upstream positions at center + forward * 1,000,000 initially for culling,
            // then tightens near/far to the actual caster depth range (lines 190-197).
            shadowCamNode->setRotation(shadowRotation);
            shadowCamNode->setPosition(center);
            shadowCamNode->translateLocal(0.0f, 0.0f, 1000000.0f);

            // Set orthographic projection to encompass the cascade's bounding sphere.
            shadowCam->setProjection(ProjectionType::Orthographic);
            shadowCam->setOrthoHeight(radius);
            shadowCam->setNearClip(0.01f);
            shadowCam->setFarClip(2000000.0f);
            shadowCam->setAspectRatio(1.0f);

            // Tighten shadow camera near/far to the depth range of the visible
            // CASTER AABB. The caster set is rotation-invariant for static
            // scenes, so the resulting depth range is identical from frame to
            // frame and stored EVSM moments don't drift between frames — this
            // is what eliminates the wing-tip flicker that the old
            // frustum-corner depth produced. It also catches casters above the
            // cascade slice (e.g. wing tips that sit higher than the
            // ground-area the cascade covers but cast shadows into it).
            //
            // Algorithm matches upstream shadow-renderer-directional.js:
            //   1. cull casters against the wide ortho frustum just set up
            //      (orthoHeight=radius, farClip=2e6 — captures everything
            //      laterally inside the cascade and at any depth);
            //   2. union the AABBs of visible casters;
            //   3. project the resulting world-space AABB's 8 corners onto the
            //      shadow camera Z axis to get min/max view-space depth;
            //   4. translate the shadow camera so the near plane sits just
            //      behind the nearest caster, set farClip to the depth span.
            {
                const Matrix4 shadowCamView = shadowCamNode->worldTransform().inverse();

                bool haveAabb = false;
                BoundingBox visibleSceneAabb;
                visibleSceneAabb.setCenter(0.0f, 0.0f, 0.0f);
                visibleSceneAabb.setHalfExtents(0.0f, 0.0f, 0.0f);

                for (auto* renderComponent : RenderComponent::instances()) {
                    if (!shouldRenderShadowRenderComponent(renderComponent, camera)) {
                        continue;
                    }
                    for (auto* meshInstance : renderComponent->meshInstances()) {
                        if (!meshInstance || !meshInstance->visible()) {
                            continue;
                        }
                        if (!shouldRenderShadowMeshInstance(meshInstance, shadowCam)) {
                            continue;
                        }
                        const auto worldAabb = meshInstance->aabb();
                        if (!haveAabb) {
                            visibleSceneAabb = worldAabb;
                            haveAabb = true;
                        } else {
                            visibleSceneAabb.add(worldAabb);
                        }
                    }
                }

                // No visible casters: keep the wide camera as-is. The shadow
                // pass will be a no-op anyway.
                if (haveAabb) {
                    const Vector3 c = visibleSceneAabb.center();
                    const Vector3 h = visibleSceneAabb.halfExtents();
                    const Vector3 corners[8] = {
                        c + Vector3(-h.getX(), -h.getY(), -h.getZ()),
                        c + Vector3(+h.getX(), -h.getY(), -h.getZ()),
                        c + Vector3(-h.getX(), +h.getY(), -h.getZ()),
                        c + Vector3(+h.getX(), +h.getY(), -h.getZ()),
                        c + Vector3(-h.getX(), -h.getY(), +h.getZ()),
                        c + Vector3(+h.getX(), -h.getY(), +h.getZ()),
                        c + Vector3(-h.getX(), +h.getY(), +h.getZ()),
                        c + Vector3(+h.getX(), +h.getY(), +h.getZ()),
                    };

                    float depthMin = 1e30f;
                    float depthMax = -1e30f;
                    for (int i = 0; i < 8; ++i) {
                        const float z = shadowCamView.transformPoint(corners[i]).getZ();
                        if (z < depthMin) depthMin = z;
                        if (z > depthMax) depthMax = z;
                    }

                    shadowCamNode->translateLocal(0.0f, 0.0f, depthMax + 0.1f);
                    shadowCam->setFarClip(depthMax - depthMin + 0.2f);
                }
            }

            // Build the viewport-scaled shadow matrix for this cascade:
            // shadowMatrix = viewportMatrix × shadowCamProj × shadowCamView
            // The viewport matrix maps NDC to the cascade's sub-region of the atlas.
            const Matrix4 shadowView = shadowCamNode->worldTransform().inverse();
            const Matrix4 shadowVP = shadowCam->projectionMatrix() * shadowView;

            const Vector4& vp = light->cascadeViewports()[cascade];
            // upstream Mat4.setViewport: maps clip coords to viewport sub-region.
            // Metal texture origin is top-left (vs OpenGL bottom-left), which flips the
            // Y axis. This is handled by negating the Y scale; the Y translate stays the
            // same as upstream because the viewport coordinates already correspond to
            // Metal's top-left-origin coordinate system. Remaps NDC [-1,1] → [0,1] for Z.
            Matrix4 viewportMatrix = Matrix4::identity();
            viewportMatrix.setElement(0, 0, vp.getZ() * 0.5f);                         // X: scale by width/2
            viewportMatrix.setElement(3, 0, vp.getX() + vp.getZ() * 0.5f);              // X: translate to region center
            // Metal: negative Y scale maps NDC Y to top-down texture V; the translate
            // is the same as upstream (no extra 1-y flip needed) because Metal viewport
            // and texture coordinates share the same top-left origin.
            viewportMatrix.setElement(1, 1, -vp.getW() * 0.5f);                         // Y: scale by -height/2 (Metal Y flip)
            viewportMatrix.setElement(3, 1, vp.getY() + vp.getW() * 0.5f);              // Y: translate to region center
            // Z: map from OpenGL NDC [-1,1] to Metal [0,1]
            // Note: shadow-vertex.metal applies clip.z = 0.5*(clip.z+clip.w), so depth
            // is already in [0,1] after vertex shader. The projection matrix produces
            // OpenGL NDC z in [-1,1], but we bake the [0,1] mapping here since the
            // shader reads the final shadow depth directly.
            viewportMatrix.setElement(2, 2, 0.5f);                                      // Z: scale by 0.5
            viewportMatrix.setElement(3, 2, 0.5f);                                      // Z: bias by 0.5

            const Matrix4 shadowMatrix = viewportMatrix * shadowVP;

            // Store in the light's matrix palette (column-major, 16 floats per cascade).
            //_shadowMatrixPalette.set(data, face * 16).
            // Matrix4 is 64 bytes on all SIMD backends — memcpy directly (same H1 fix
            // as SkinBatchInstance::updateMatrices).
            float* palette = light->shadowMatrixPaletteData();
            std::memcpy(&palette[cascade * 16], &shadowMatrix, 64);
        }
    }

    std::shared_ptr<RenderPass> ShadowRendererDirectional::getLightRenderPass(Light* light, Camera* camera,
        const int face, const bool clearRenderTarget, const bool allCascadesRendering)
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

        auto renderPass = std::make_shared<RenderPassShadowDirectional>(_device, _shadowRenderer, light, camera, shadowCamera, face,
            allCascadesRendering);
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
                auto renderPass = getLightRenderPass(light, camera, 0, true, true);
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
                        // Pass 1 — horizontal: shadowTexture → blurTemp.
                        auto blurH = std::make_shared<RenderPassVsmBlur>(_device,
                            sm->shadowTexture(), sm->blurTempRenderTarget(),
                            resolution, true, filterSize);
                        // Pass 2 — vertical: blurTemp → shadowTexture.
                        auto blurV = std::make_shared<RenderPassVsmBlur>(_device,
                            sm->blurTempTexture(), sm->renderTargets()[0],
                            resolution, false, filterSize);
                        frameGraph->addRenderPass(blurH);
                        frameGraph->addRenderPass(blurV);
                    }
                }
            }
        }
    }
}
