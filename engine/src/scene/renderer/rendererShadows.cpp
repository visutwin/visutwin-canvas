// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// Renderer: one-shot shadow requests and the directional shadow-map fits.
#include "rendererInternal.h"

namespace visutwin::canvas
{
    using namespace renderer_internal;

    void Renderer::consumeOneShotShadows()
    {
        // No shadow renders in a precompile frame: the requests wait for the frame that does.
        if (_precompileFrame) {
            return;
        }
        const bool clusteredEnabled = _scene && _scene->clusteredLightingEnabled();
        auto consume = [&](Light* light) {
            if (!light || !_shadowRenderer || !_shadowRenderer->needsShadowRendering(light)) {
                return;
            }
            _device->frameCounters().shadowMapUpdates += light->numShadowFaces();
            if (light->shadowUpdateMode() == ShadowUpdateType::SHADOWUPDATE_THISFRAME) {
                light->setShadowUpdateMode(ShadowUpdateType::SHADOWUPDATE_NONE);
            }
        };

        // Local lights. Under clustered lighting a spot also needs an atlas slot to
        // have been allocated, or no pass was built for it and its request stands.
        for (auto* lightComponent : instancesOf<LightComponent>(componentRegistry())) {
            if (!lightComponent || !lightComponent->active() ||
                lightComponent->type() == LightType::LIGHTTYPE_DIRECTIONAL) {
                continue;
            }
            Light* sceneLight = lightComponent->light();
            if (clusteredEnabled && sceneLight && !sceneLight->atlasViewportAllocated()) {
                continue;
            }
            consume(sceneLight);
        }

        // A directional light's shadow is fit and rendered per camera, so its lights
        // are reached through the per-camera map rather than the component list.
        for (const auto& [cullCamera, cameraLights] : _cameraDirShadowLights) {
            (void)cullCamera;
            for (Light* light : cameraLights) {
                consume(light);
            }
        }
    }

    void Renderer::cullShadowmaps(Camera* camera, const CameraComponent* cameraComponent, const bool designated)
    {
        // Refresh only this camera's entry — buildFrameGraph calls this once per
        // unique camera, and clearing the whole map here would wipe the entries
        // of previously culled cameras (breaking directional shadows whenever
        // more than one camera renders).
        _cameraDirShadowLights.erase(camera);

        if (!camera || !_shadowRendererDirectional) {
            return;
        }

        std::vector<Light*> dirShadowLights;

        for (auto* lightComponent : instancesOf<LightComponent>(componentRegistry())) {
            // active(), not enabled(): a light on a disabled entity must stop lighting.
            if (!lightComponent || !lightComponent->active()) {
                continue;
            }
            if (lightComponent->type() != LightType::LIGHTTYPE_DIRECTIONAL || !lightComponent->castShadows()) {
                continue;
            }

            Light* sceneLight = lightComponent->light();
            if (!sceneLight) {
                continue;
            }
            // A shadow with shadow bounds is fitted once for every camera
            // (cullBoundedShadowmaps).
            if (sceneLight->shadowBounds()) {
                continue;
            }
            if (!designated) {
                if (sceneLight->shadowUpdateMode() != ShadowUpdateType::SHADOWUPDATE_REALTIME) {
                    continue;
                }
                const bool lightsCameraLayer = !cameraComponent ||
                    std::ranges::any_of(lightComponent->layers(),
                        [cameraComponent](const int layer) { return cameraComponent->rendersLayer(layer); });
                if (!lightsCameraLayer) {
                    continue;
                }
            }

            // Allocate shadow map if not yet created; the light owns it.
            if (!sceneLight->shadowMap()) {
                sceneLight->setShadowMap(ShadowMap::create(_device.get(), sceneLight));
            }

            if (!sceneLight->shadowMap()) {
                continue;
            }

            // Set up the shadow camera (position, projection, snap) — unless the light
            // is at SHADOWUPDATE_NONE, whose map was rendered once and will not be
            // again. The fit follows the CAMERA (the cascades are sliced from its
            // frustum), and this port writes the sampling matrices — the palette and
            // the per-cascade fit — during the cull, then reads them at bind time. So a
            // re-fit here would move every sampling matrix with the view while the
            // texture stayed put, and a one-shot shadow came out wrong the moment the
            // camera moved. Writing the shadow matrix inside the shadow pass would
            // keep, under NONE, the fit the texture was rendered with; skipping the
            // fit is the same invariant reached from the other side. The light still joins the list:
            // the forward pass reads its matrices from it, and the pass builder and the
            // one-shot consume both gate on needsShadowRendering themselves.
            if (sceneLight->shadowUpdateMode() != ShadowUpdateType::SHADOWUPDATE_NONE) {
                _shadowRendererDirectional->cull(sceneLight, camera);
                sceneLight->storeDirectionalFit(camera);
            }

            dirShadowLights.push_back(sceneLight);
        }

        if (!dirShadowLights.empty()) {
            _cameraDirShadowLights[camera] = std::move(dirShadowLights);
        }
    }

    void Renderer::cullBoundedShadowmaps()
    {
        _cameraDirShadowLights.erase(nullptr);
        if (!_shadowRendererDirectional) {
            return;
        }
        std::vector<Light*> lights;
        for (auto* lightComponent : instancesOf<LightComponent>(componentRegistry())) {
            if (!lightComponent || !lightComponent->active() ||
                lightComponent->type() != LightType::LIGHTTYPE_DIRECTIONAL || !lightComponent->castShadows()) {
                continue;
            }
            Light* sceneLight = lightComponent->light();
            if (!sceneLight || !sceneLight->shadowBounds()) {
                continue;
            }
            if (!sceneLight->shadowMap()) {
                sceneLight->setShadowMap(ShadowMap::create(_device.get(), sceneLight));
            }
            if (!sceneLight->shadowMap()) {
                continue;
            }
            // A cached shadow keeps the fit it was rendered with (see cullShadowmaps).
            if (sceneLight->shadowUpdateMode() != ShadowUpdateType::SHADOWUPDATE_NONE) {
                _shadowRendererDirectional->cullBounds(sceneLight);
                sceneLight->storeDirectionalFit(nullptr);
            }
            lights.push_back(sceneLight);
        }
        if (!lights.empty()) {
            _cameraDirShadowLights[nullptr] = std::move(lights);
        }
    }

    void Renderer::resolveAutoAspectRatio(Camera* camera) const
    {
        if (!camera || camera->aspectRatioMode() != AspectRatioMode::ASPECT_AUTO || !_device ||
            camera->multiView()) {
            return;
        }
        const auto* target = camera->renderTarget().get();
        const int targetWidth = std::max(target ? target->width() : _device->size().first, 1);
        const int targetHeight = std::max(target ? target->height() : _device->size().second, 1);
        const PixelRect viewport = normalizedRectToPixels(camera->rect(), targetWidth, targetHeight);
        camera->setAspectRatio(static_cast<float>(viewport.w) / static_cast<float>(viewport.h));
    }
}
