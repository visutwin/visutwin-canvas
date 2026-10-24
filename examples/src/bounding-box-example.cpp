// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.10.2026
//
// Port of upstream gizmos/bounding-box.
//
// Corner brackets, dashed edges and a faint volume outline the bounds of a default white box at
// the origin, faint where the box hides them: a BoundingBoxGizmo in a layer that keeps the scene's
// depth, over the default infinite Grid, the box lit by a default directional light at euler
// (0, 0, -60) with ambient 0.2. Once large on screen, face and corner handles resize the box,
// labelled with its width, height and depth, and a marker shows its origin. The camera starts at
// 4 x its aspect on each axis, orbiting the origin (zoom 2..10, pitch +/-89.999, damping 0.95, no
// fly), and stops orbiting while a handle is dragged.
//
// Keys stand in for upstream's controls panel:
//   [ / ]  arm length -/+ 0.05 (0.05..1)
//   - / =  arm thickness -/+ 0.5 (1..10)
//   , / .  padding -/+ 0.01 (0..0.5)
//   O      occluded colour alpha, cycled 0.2 / 0.5 / 0
//   D      dashed edges on / off
//   F      face handles on / off
//   C      corner handles on / off
//   E      resize centred on / off
//   U      corner handles scale uniformly on / off
//   H      handles on / off
//   R      readout on / off
//   I      origin marker on / off
//   S      snap on / off (lengths by 1)
//   P      projection, perspective / orthographic
//   9 / 0  fov -/+ 5 (30..100)
//
// DEVIATIONS:
// - No controls panel; the occluded alpha is cycled by a key instead of the colour pickers.
// - Upstream's Grid script is the Grid extra used directly (Grid::attach).
// - The camera controls have no `sceneSize` here; with fly off it changes nothing upstream shows.
//
#include <algorithm>
#include <memory>

#include "../overlayGizmoExample.h"
#include "framework/gizmo/boundingBoxGizmo.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class BoundingBoxExample final : public OverlayGizmoExample
{
public:
    BoundingBoxExample() : OverlayGizmoExample("Bounding Box Gizmo") {}

protected:
    bool create() override
    {
        if (!createStage(4.0f, Vector2(2.0f, 10.0f), "Bounds")) {
            return false;
        }

        _boxMaterial = std::make_shared<StandardMaterial>();
        auto* box = createPrimitive("box", _boxMaterial.get());

        _gizmo = std::make_unique<BoundingBoxGizmo>(_camera, _layer);
        watch(_gizmo.get());
        _gizmo->attach(box);

        spdlog::info("Keys: [ ] arm length | - = thickness | , . padding | O occluded alpha | D edges | "
                     "F face | C corner | E centred | U uniform | H handles | R readout | I origin | S snap | "
                     "P projection | 9 0 fov");
        return true;
    }

    void update(float /*dt*/) override
    {
        handleKeys();
        updateStage(_gizmo ? _gizmo->labels() : std::vector<GizmoReadout>{});
    }

    void destroy() override
    {
        _gizmo.reset();
        destroyStage();
    }

private:
    void handleKeys()
    {
        const auto* keyboard = engine()->keyboard();
        Camera* camera = _camera ? _camera->camera() : nullptr;
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
        if (keyboard->wasPressed(Key::D)) _gizmo->edges = !_gizmo->edges;
        if (keyboard->wasPressed(Key::F)) _gizmo->faceHandles = !_gizmo->faceHandles;
        if (keyboard->wasPressed(Key::C)) _gizmo->cornerHandles = !_gizmo->cornerHandles;
        if (keyboard->wasPressed(Key::E)) _gizmo->centered = !_gizmo->centered;
        if (keyboard->wasPressed(Key::U)) _gizmo->uniform = !_gizmo->uniform;
        if (keyboard->wasPressed(Key::H)) _gizmo->handles = !_gizmo->handles;
        if (keyboard->wasPressed(Key::R)) _gizmo->readout = !_gizmo->readout;
        if (keyboard->wasPressed(Key::I)) _gizmo->icon = !_gizmo->icon;
        if (keyboard->wasPressed(Key::S)) _gizmo->snap = !_gizmo->snap;
        if (keyboard->wasPressed(Key::P)) toggleProjection();
        if (keyboard->wasPressed(Key::Digit9)) camera->setFov(std::max(camera->fov() - 5.0f, 30.0f));
        if (keyboard->wasPressed(Key::Digit0)) camera->setFov(std::min(camera->fov() + 5.0f, 100.0f));
    }

    std::shared_ptr<StandardMaterial> _boxMaterial;
    std::unique_ptr<BoundingBoxGizmo> _gizmo;
    int _occludedAlpha = 0;
};

VISUTWIN_EXAMPLE_MAIN(BoundingBoxExample)
