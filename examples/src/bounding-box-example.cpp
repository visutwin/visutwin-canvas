// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.10.2026
//
// Port of upstream gizmos/bounding-box.
//
// Corner brackets outline the bounds of a default white box at the origin, faint where the box
// hides them: a BoundingBoxGizmo drawn in a layer that keeps the scene's depth, over the default
// infinite Grid, the box lit by a default directional light at euler (0, 0, -60) with ambient 0.2. The camera
// starts at 4 x its aspect on each axis, orbiting the origin (zoom 2..10, pitch +/-89.999,
// damping 0.95, no fly).
//
// Keys stand in for upstream's controls panel:
//   [ / ]  arm length -/+ 0.05 (0.05..1)
//   - / =  arm thickness -/+ 0.5 (1..10)
//   , / .  padding -/+ 0.01 (0..0.5)
//   O      occluded colour alpha, cycled 0.2 / 0.5 / 0
//   P      projection, perspective / orthographic
//   9 / 0  fov -/+ 5 (30..100)
//
// DEVIATIONS:
// - No controls panel; the theme is cycled by a key instead of two colour pickers.
// - Upstream's Grid script is the Grid extra used directly (Grid::attach).
// - The camera controls have no `sceneSize` here; with fly off it changes nothing upstream shows.
//
#include <algorithm>
#include <memory>

#include "../exampleApp.h"
#include "extras/grid/grid.h"
#include "extras/script/cameraControls.h"
#include "framework/gizmo/boundingBoxGizmo.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class BoundingBoxExample final : public ExampleApp
{
public:
    BoundingBoxExample() : ExampleApp({.title = "Bounding Box Gizmo"}) {}

protected:
    bool create() override
    {
        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

        _boxMaterial = std::make_shared<StandardMaterial>();
        auto* box = createPrimitive("box", _boxMaterial.get());

        _cameraEntity = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        auto* camera = _cameraEntity->findComponent<CameraComponent>();
        if (!camera || !camera->camera()) {
            spdlog::error("Failed to create camera");
            return false;
        }
        camera->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
        camera->camera()->setFarClip(1000.0f);
        const float cameraOffset = 4.0f * camera->camera()->aspectRatio();
        _cameraEntity->setPosition(Vector3(cameraOffset, cameraOffset, cameraOffset));

        if (auto* controls = addOrbitControls(_cameraEntity, Vector3(0.0f, 0.0f, 0.0f))) {
            controls->setRotateDamping(0.95f);
            controls->setMoveDamping(0.95f);
            controls->setZoomDamping(0.95f);
            controls->setPitchRange(Vector2(-89.999f, 89.999f));
            controls->setZoomRange(Vector2(2.0f, 10.0f));
            controls->setEnableFly(false);
            controls->storeResetState();
        }

        auto* light = new Entity();
        light->setEngine(engine());
        light->addComponent<LightComponent>();
        root()->addChild(light);
        light->setLocalEulerAngles(0.0f, 0.0f, -60.0f);

        // The gizmo's layer keeps the scene depth, so the hidden parts draw in the occluded colour.
        _layer = Gizmo::createLayer(engine(), "Bounds");
        _layer->setClearDepthBuffer(false);
        _gizmo = std::make_unique<BoundingBoxGizmo>(camera, _layer);
        _gizmo->attach(box);

        auto* gridEntity = new Entity();
        gridEntity->setEngine(engine());
        gridEntity->setName("grid");
        root()->addChild(gridEntity);
        _grid = std::make_unique<Grid>(device());
        _grid->attach(gridEntity);

        spdlog::info("Keys: [ ] arm length | - = thickness | , . padding | O occluded alpha | "
                     "P projection | 9 0 fov");
        return true;
    }

    void update(float /*dt*/) override
    {
        handleKeys();
    }

    void destroy() override
    {
        _gizmo.reset();
        _grid.reset();
    }

private:
    void handleKeys()
    {
        const auto* keyboard = engine()->keyboard();
        auto* cameraComponent = _cameraEntity ? _cameraEntity->findComponent<CameraComponent>() : nullptr;
        Camera* camera = cameraComponent ? cameraComponent->camera() : nullptr;
        if (!keyboard || !_gizmo || !camera) {
            return;
        }
        const auto step = [&](const Key down, const Key up, float& value, const float delta, const float lo,
            const float hi) {
            if (keyboard->wasPressed(down)) value = std::clamp(value - delta, lo, hi);
            if (keyboard->wasPressed(up)) value = std::clamp(value + delta, lo, hi);
        };
        step(Key::LeftBracket, Key::RightBracket, _gizmo->armLength, 0.05f, 0.05f, 1.0f);
        step(Key::Minus, Key::Equals, _gizmo->armThickness, 0.5f, 1.0f, 10.0f);
        step(Key::Comma, Key::Period, _gizmo->padding, 0.01f, 0.0f, 0.5f);

        if (keyboard->wasPressed(Key::O)) {
            constexpr float alphas[] = {0.2f, 0.5f, 0.0f};
            _occludedAlpha = (_occludedAlpha + 1) % 3;
            Color occluded = _gizmo->theme().occluded;
            occluded.a = alphas[_occludedAlpha];
            _gizmo->setTheme({.occluded = occluded});
        }
        if (keyboard->wasPressed(Key::P)) {
            camera->setProjection(camera->projection() == ProjectionType::Perspective
                ? ProjectionType::Orthographic : ProjectionType::Perspective);
        }
        if (keyboard->wasPressed(Key::Digit9)) camera->setFov(std::max(camera->fov() - 5.0f, 30.0f));
        if (keyboard->wasPressed(Key::Digit0)) camera->setFov(std::min(camera->fov() + 5.0f, 100.0f));
    }

    std::shared_ptr<StandardMaterial> _boxMaterial;
    std::shared_ptr<Layer> _layer;
    std::unique_ptr<BoundingBoxGizmo> _gizmo;
    std::unique_ptr<Grid> _grid;
    Entity* _cameraEntity = nullptr;
    int _occludedAlpha = 0;
};

VISUTWIN_EXAMPLE_MAIN(BoundingBoxExample)
