// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.10.2026
//
// Port of upstream test/gsplat-shadow-orientation.
//
// The orientation of gaussian splat footprints in a shadow map. Six long, thin splats
// (scale (0.5, 0.015, 0.015), opaque) float 1.5 above a 30 x 30 ground plane (diffuse 0.8,
// casting nothing), 1.4 apart along X, each turned about the vertical axis by 0, 30, 60, 90,
// 120 and 150 degrees and coloured from red to yellow (1, 0.3 + 0.7 i / 5, 0.1). A horizontal
// needle casts a shadow parallel to itself for any light direction, so under the white
// directional light at (35, 0, 0) (2048 PCF3 shadow, bias 0.1, normal bias 0.05, distance 20)
// every shadow must lie parallel to the needle above it, on both backends. Ambient 0.4. The
// camera (fov 45, clear (0.2, 0.2, 0.25)) looks from (0, 7.8, 6.5) at (0, 0.75, -0.55) and
// orbits it, zooming from 2 to 20. The back buffer is not multisampled.
//
// No keys beyond the orbit camera's (drag to orbit, wheel to zoom, R to reset).
//
// DEVIATIONS:
// - upstream reads the needles through a custom splat format (centre and angle, colour) whose
//   shader functions return the scale and rotation; here the same values are built on the CPU
//   as activated splats (GSplatData::fromActivated).
// - upstream's control panel picks its splat renderer (CPU or GPU sort); this engine has the
//   one, so there is nothing to choose.
// - upstream caps the pixel ratio at 2; the examples render at one pixel per point.
//
#include <cmath>
#include <memory>
#include <vector>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/components/gsplat/gsplatComponent.h"
#include "framework/components/gsplat/gsplatComponentSystem.h"
#include "scene/constants.h"
#include "scene/gsplat/gsplatResource.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float kNeedleAngles[] = {0.0f, 30.0f, 60.0f, 90.0f, 120.0f, 150.0f};
    constexpr float kSpacing = 1.4f;
    constexpr float kHeight = 1.5f;

    // The degree-0 spherical-harmonic coefficient that displays as `colour`.
    float shDc(const float colour)
    {
        constexpr float kShC0 = 0.28209479177387814f;
        return (colour - 0.5f) / kShC0;
    }
}

class GsplatShadowOrientationExample final : public ExampleApp
{
public:
    GsplatShadowOrientationExample() : ExampleApp({.title = "Gaussian Splat Shadow Orientation",
        .antialias = false}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<GSplatComponentSystem>();
    }

    bool create() override
    {
        // The needles: centred along X, each turned about Y by its angle.
        constexpr size_t count = std::size(kNeedleAngles);
        GSplatData::ActivatedSplats splats;
        splats.count = count;
        for (size_t i = 0; i < count; ++i) {
            const float x = (static_cast<float>(i) - static_cast<float>(count - 1) * 0.5f) * kSpacing;
            const float halfAngle = 0.5f * kNeedleAngles[i] * DEG_TO_RAD;
            const float green = 0.3f + 0.7f * static_cast<float>(i) / static_cast<float>(count - 1);
            splats.positions.insert(splats.positions.end(), {x, kHeight, 0.0f});
            splats.rotations.insert(splats.rotations.end(), {0.0f, std::sin(halfAngle), 0.0f, std::cos(halfAngle)});
            splats.scales.insert(splats.scales.end(), {0.5f, 0.015f, 0.015f});
            splats.opacities.push_back(1.0f);
            splats.sh0.insert(splats.sh0.end(), {shDc(1.0f), shDc(green), shDc(0.1f)});
        }
        auto data = GSplatData::fromActivated(splats, "needles");
        if (!data) {
            spdlog::error("gsplat-shadow-orientation: the needles could not be built");
            return false;
        }
        auto resource = std::make_shared<GSplatResource>(std::move(data), device());

        auto* needles = new Entity();
        needles->setName("needles");
        needles->setEngine(engine());
        root()->addChild(needles);
        auto* gsplat = static_cast<GSplatComponent*>(needles->addComponent<GSplatComponent>());
        gsplat->setResource(resource);
        gsplat->setCastShadows(true);

        // The ground plane receiving the shadows
        _groundMaterial = std::make_shared<StandardMaterial>();
        _groundMaterial->setDiffuse(Color(0.8f, 0.8f, 0.8f, 1.0f));
        auto* ground = createPrimitive("plane", _groundMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(30.0f, 1.0f, 30.0f));
        if (auto* render = ground->findComponent<RenderComponent>()) {
            render->setCastShadows(false);
        }

        // The light, tilted so the shadows land beside the needles rather than under them
        auto* light = createDirectionalLight(Vector3(35.0f, 0.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* lightComponent = light->findComponent<LightComponent>()) {
            lightComponent->setShadowBias(0.1f);
            lightComponent->setShadowNormalBias(0.05f);
            lightComponent->setShadowDistance(20.0f);
            lightComponent->setShadowResolution(2048);
            lightComponent->setShadowType(ShadowType::SHADOW_PCF3_32F);
        }
        scene()->setAmbientLight(0.4f, 0.4f, 0.4f);

        // The camera, looking at the needles from 45 degrees up
        const Vector3 focus(0.0f, 0.75f, -0.55f);
        Entity* camera = createCamera(Vector3(0.0f, 7.8f, 6.5f));
        camera->lookAt(focus);
        if (auto* cameraComponent = camera->findComponent<CameraComponent>()) {
            cameraComponent->camera()->setClearColor(Color(0.2f, 0.2f, 0.25f, 1.0f));
            cameraComponent->camera()->setFov(45.0f);
        }
        if (CameraControls* controls = addOrbitControls(camera, focus)) {
            controls->setZoomRange(Vector2(2.0f, 20.0f));
            controls->storeResetState();
        }
        return true;
    }

private:
    std::shared_ptr<StandardMaterial> _groundMaterial;
};

VISUTWIN_EXAMPLE_MAIN(GsplatShadowOrientationExample)
