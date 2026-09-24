// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream loaders/glb.
//
// Loads geometry-camera-light.glb, a grey cube on a scaled plane that also carries
// two cameras (one perspective, one orthographic) and two KHR_lights_punctual
// lights (a blue point light and an orange spot). The glb's cameras start
// disabled; each gets automatic aspect ratio and a physical exposure of aperture
// 4, shutter 1/100 and sensitivity 500, the lights are all enabled, and the active
// camera switches every two seconds.
//
// DEVIATION: the engine has no camera aperture, shutter or sensitivity, so the
// physical exposure upstream's cameras compute from them,
// 1 / (1.2 * 2^log2(N^2 / t * 100 / S)), is set as the scene exposure instead.
// Both cameras share those settings, so a scene-wide exposure is exact.
//
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "scene/camera.h"

using namespace visutwin::canvas;

namespace
{
    constexpr const char* kModel = "models/geometry-camera-light.glb";

    // Upstream's physical camera settings for the glb's cameras.
    constexpr float kAperture = 4.0f;
    constexpr float kShutter = 1.0f / 100.0f;
    constexpr float kSensitivity = 500.0f;

    constexpr float kSwitchInterval = 2.0f;

    /// Upstream Camera::getExposure.
    float physicalExposure(const float aperture, const float shutter, const float sensitivity)
    {
        const float ev100 = std::log2((aperture * aperture) / shutter * 100.0f / sensitivity);
        return 1.0f / (std::pow(2.0f, ev100) * 1.2f);
    }
}

class GlbLoaderExample final: public ExampleApp
{
public:
    GlbLoaderExample(): ExampleApp({.title = "GLB"}) {}

protected:
    bool create() override
    {
        _asset = std::make_unique<Asset>("scene", AssetType::CONTAINER, assetPath(kModel));
        const auto resource = _asset->resource();
        if (!resource || !std::holds_alternative<ContainerResource*>(*resource) ||
            !std::get<ContainerResource*>(*resource)) {
            spdlog::error("Failed to load {}", kModel);
            return false;
        }

        // Create an instance using render component.
        Entity* entity = std::get<ContainerResource*>(*resource)->instantiateRenderEntity();
        if (!entity) {
            spdlog::error("Failed to instantiate {}", kModel);
            return false;
        }
        entity->setEngine(engine());
        root()->addChild(entity);

        // glb lights use physical units.
        scene()->setPhysicalUnits(true);
        // The exposure upstream's cameras would compute (see header).
        scene()->setExposure(physicalExposure(kAperture, kShutter, kSensitivity));

        // Find all cameras - by default they are disabled.
        _cameras = entity->findComponents<CameraComponent>();
        for (auto* component : _cameras) {
            // Set the aspect ratio to automatic to work with any window size.
            component->camera()->setAspectRatioMode(AspectRatioMode::ASPECT_AUTO);
        }

        // Enable all lights from the glb.
        for (auto* component : entity->findComponents<LightComponent>()) {
            component->setEnabled(true);
        }

        spdlog::info("GLB: {} cameras, {} lights, exposure {}", _cameras.size(),
            entity->findComponents<LightComponent>().size(), scene()->exposure());
        if (_cameras.empty()) {
            spdlog::error("{} has no cameras", kModel);
            return false;
        }
        return true;
    }

    void update(const float dt) override
    {
        _time -= dt;

        // Change the camera every few seconds.
        if (_time <= 0.0f) {
            _time = kSwitchInterval;

            // Disable current camera.
            _cameras[_activeCamera]->setEnabled(false);

            // Activate next camera.
            _activeCamera = (_activeCamera + 1) % _cameras.size();
            _cameras[_activeCamera]->setEnabled(true);
        }
    }

private:
    std::unique_ptr<Asset> _asset;
    std::vector<CameraComponent*> _cameras;
    size_t _activeCamera = 0;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(GlbLoaderExample)
