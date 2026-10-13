// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// The shape several material examples share: one glTF model placed in the scene, lit by an
// environment atlas (and optionally a directional light), seen by an orbit camera. Upstream
// builds these with its orbit-camera script and no focus entity, which frames the whole scene:
// the pivot is the centre of the scene's bounds, the camera first looks at it from where it was
// authored (which sets the pitch), and the example then sets the yaw and the distance, or keeps
// the framing distance (1.5 x the largest half extent over sin(fov / 2), clamped to the script's
// maximum). This starts the examples' CameraControls in orbit mode at that pose.
//
#pragma once

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>

#include "exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/light/lightComponent.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"

namespace visutwin::canvas
{
    struct GltfShowcaseOptions
    {
        std::string title;
        std::string model;                         // under assets/
        std::string envAtlas = "cubemaps/helipad-env-atlas.png";
        float skyboxIntensity = 1.0f;
        int skyboxMip = 0;
        float skyboxYaw = 0.0f;                    // the skybox rotation about Y, degrees

        Vector3 modelEuler = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 modelPosition = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 modelScale = Vector3(1.0f, 1.0f, 1.0f);

        Vector3 cameraPosition = Vector3(0.0f, 0.0f, 0.0f);   // where the camera is authored
        std::optional<float> cameraYaw;            // set after the framing, degrees
        std::optional<float> cameraDistance;       // set after the framing; unset keeps it
        float distanceMax = 1e6f;                  // the orbit script's distanceMax
        float fov = 45.0f;
        float nearClip = 0.1f;
        float farClip = 1000.0f;
        std::optional<Color> clearColor;
        int toneMapping = TONEMAP_ACES;
        bool sceneColorMap = false;
        // Move the depth layer (where the colour grab happens) to this opaque index.
        std::optional<int> depthLayerOpaqueIndex;

        // An optional directional light
        std::optional<Color> lightColor;
        float lightIntensity = 1.0f;
        Vector3 lightEuler = Vector3(45.0f, 180.0f, 0.0f);
    };

    class GltfShowcaseExample : public ExampleApp
    {
    public:
        explicit GltfShowcaseExample(GltfShowcaseOptions options)
            : ExampleApp({.title = options.title}), _options(std::move(options)) {}

    protected:
        bool create() override
        {
            const GltfShowcaseOptions& o = _options;
            _envAtlasAsset = std::make_unique<Asset>("env-atlas", AssetType::TEXTURE, assetPath(o.envAtlas),
                AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
            _modelAsset = std::make_unique<Asset>("model", AssetType::CONTAINER, assetPath(o.model));
            if (Texture* envAtlas = _envAtlasAsset->resourceAs<Texture>()) {
                scene()->setEnvAtlas(envAtlas);
            }
            scene()->setSkyboxIntensity(o.skyboxIntensity);
            scene()->setSkyboxMip(o.skyboxMip);
            scene()->setSkyboxRotation(Quaternion::fromEulerAngles(0.0f, o.skyboxYaw, 0.0f));

            if (o.depthLayerOpaqueIndex) {
                const auto layers = scene()->layers();
                if (auto depthLayer = layers->getLayerById(LAYERID_DEPTH)) {
                    layers->remove(depthLayer);
                    layers->insertOpaque(depthLayer, *o.depthLayerOpaqueIndex);
                }
            }

            auto* container = _modelAsset->resourceAs<ContainerResource>();
            if (!container) {
                spdlog::error("{} needs {}", o.title, o.model);
                return false;
            }
            _model = container->instantiateRenderEntity();
            _model->setLocalEulerAngles(o.modelEuler.getX(), o.modelEuler.getY(), o.modelEuler.getZ());
            _model->setPosition(o.modelPosition);
            _model->setLocalScale(o.modelScale.getX(), o.modelScale.getY(), o.modelScale.getZ());
            root()->addChild(_model);

            // The orbit script's start: the pivot at the scene's centre, the pitch from looking at it
            // from the authored position, then the example's yaw and distance.
            const BoundingBox bounds = entityBounds(_model);
            const Vector3 pivot = bounds.center();
            const Vector3 toPivot = (pivot - o.cameraPosition).normalized();
            const float pitch = std::clamp(std::asin(std::clamp(toPivot.getY(), -1.0f, 1.0f)) * RAD_TO_DEG,
                -90.0f, 90.0f);
            const float yaw = o.cameraYaw.value_or(std::atan2(-toPivot.getX(), -toPivot.getZ()) * RAD_TO_DEG);
            const Vector3 half = bounds.halfExtents();
            const float radius = std::max({half.getX(), half.getY(), half.getZ()});
            const float framed = std::min(radius * 1.5f / std::sin(0.5f * o.fov * DEG_TO_RAD), o.distanceMax);
            const float distance = std::min(o.cameraDistance.value_or(framed), o.distanceMax);
            const Vector3 forward = Quaternion::fromEulerAngles(pitch, yaw, 0.0f) * Vector3(0.0f, 0.0f, -1.0f);

            auto* camera = createCamera(pivot - forward * distance);
            _cameraComponent = camera->findComponent<CameraComponent>();
            if (!_cameraComponent || !_cameraComponent->camera()) {
                return false;
            }
            _cameraComponent->camera()->setFov(o.fov);
            _cameraComponent->camera()->setNearClip(o.nearClip);
            _cameraComponent->camera()->setFarClip(o.farClip);
            if (o.clearColor) {
                _cameraComponent->camera()->setClearColor(*o.clearColor);
            }
            _cameraComponent->setToneMapping(o.toneMapping);
            if (o.sceneColorMap) {
                _cameraComponent->requestSceneColorMap(true);
            }
            if (auto* controls = addOrbitControls(camera, pivot)) {
                controls->setZoomRange(Vector2(0.0f, o.distanceMax));
                controls->storeResetState();
            }

            if (o.lightColor) {
                createDirectionalLight(o.lightEuler, *o.lightColor, o.lightIntensity, false);
            }
            return true;
        }

        GltfShowcaseOptions _options;
        std::unique_ptr<Asset> _envAtlasAsset;
        std::unique_ptr<Asset> _modelAsset;
        Entity* _model = nullptr;
        CameraComponent* _cameraComponent = nullptr;
    };
}
