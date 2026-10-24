// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// Port of upstream gizmos/shape.
//
// The shape gizmos, one shown at a time at the origin: a box (half extents 0.5), a sphere (radius
// 0.5), a cone (radius 0.5, height 1), a capsule (radius 0.5, height 2) and a cylinder (radius 0.5,
// height 2), each with handles that drag its size, in a layer that keeps the scene depth. The
// camera stops orbiting while a handle is dragged (zoom 1..20).
//
// Keys stand in for upstream's controls panel:
//   1-5    the shape: box, sphere, cone, capsule, cylinder
//   F      box face handles on / off
//   C      box corner handles on / off
//   E      box resize centred on / off
//   U      box corner handles scale uniformly on / off
//   H      handles on / off
//   R      readout on / off
//   S      snap on / off (lengths by 1)
//   [ / ]  line thickness -/+ 0.5 (0.5..8)
//   P      projection, perspective / orthographic
// VISUTWIN_GIZMO_SHAPE=0-4 starts with that shape, for a capture.
//
// DEVIATIONS:
// - No controls panel: keys toggle the settings, and the theme keeps its defaults.
// - The camera controls have no `sceneSize` here; with fly off it changes nothing upstream shows.
//
#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>

#include "../overlayGizmoExample.h"
#include "framework/gizmo/shapeGizmos.h"

using namespace visutwin::canvas;

class ShapeGizmoExample final : public OverlayGizmoExample
{
public:
    ShapeGizmoExample() : OverlayGizmoExample("Shape Gizmo") {}

protected:
    bool create() override
    {
        if (!createStage(4.0f, Vector2(1.0f, 20.0f))) {
            return false;
        }

        _box = std::make_unique<BoxGizmo>(_camera, _layer);
        _sphere = std::make_unique<SphereGizmo>(_camera, _layer);
        _cone = std::make_unique<ConeGizmo>(_camera, _layer);
        _capsule = std::make_unique<CapsuleGizmo>(_camera, _layer);
        _cylinder = std::make_unique<CylinderGizmo>(_camera, _layer);
        _gizmos = {_box.get(), _sphere.get(), _cone.get(), _capsule.get(), _cylinder.get()};
        for (ShapeGizmo* gizmo : _gizmos) {
            watch(gizmo);
        }
        const char* shape = std::getenv("VISUTWIN_GIZMO_SHAPE");
        show(shape ? static_cast<size_t>(std::clamp(std::atoi(shape), 0, 4)) : 0);

        spdlog::info("Keys: 1-5 shape | F face handles | C corner handles | E centred | U uniform | H handles | "
                     "R readout | S snap | [ ] line thickness | P projection");
        return true;
    }

    void update(float /*dt*/) override
    {
        handleKeys();
        updateStage(_gizmos[_shown] ? _gizmos[_shown]->labels() : std::vector<GizmoReadout>{});
    }

    void destroy() override
    {
        _gizmos = {};
        _box.reset();
        _sphere.reset();
        _cone.reset();
        _capsule.reset();
        _cylinder.reset();
        destroyStage();
    }

private:
    // show one shape at a time
    void show(const size_t index)
    {
        _shown = index;
        for (size_t i = 0; i < _gizmos.size(); ++i) {
            _gizmos[i]->setEnabled(i == index);
        }
    }

    void handleKeys()
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard || !_box) {
            return;
        }
        constexpr std::array<Key, 5> digits = {Key::Digit1, Key::Digit2, Key::Digit3, Key::Digit4, Key::Digit5};
        for (size_t i = 0; i < digits.size(); ++i) {
            if (keyboard->wasPressed(digits[i])) {
                show(i);
            }
        }
        if (keyboard->wasPressed(Key::F)) _box->faceHandles = !_box->faceHandles;
        if (keyboard->wasPressed(Key::C)) _box->cornerHandles = !_box->cornerHandles;
        if (keyboard->wasPressed(Key::E)) _box->centered = !_box->centered;
        if (keyboard->wasPressed(Key::U)) _box->uniform = !_box->uniform;
        for (ShapeGizmo* gizmo : _gizmos) {
            if (keyboard->wasPressed(Key::H)) gizmo->handles = !gizmo->handles;
            if (keyboard->wasPressed(Key::R)) gizmo->readout = !gizmo->readout;
            if (keyboard->wasPressed(Key::S)) gizmo->snap = !gizmo->snap;
            if (keyboard->wasPressed(Key::LeftBracket)) {
                gizmo->setLineThickness(std::max(gizmo->lineThickness() - 0.5f, 0.5f));
            }
            if (keyboard->wasPressed(Key::RightBracket)) {
                gizmo->setLineThickness(std::min(gizmo->lineThickness() + 0.5f, 8.0f));
            }
        }
        if (keyboard->wasPressed(Key::P)) toggleProjection();
    }

    std::unique_ptr<BoxGizmo> _box;
    std::unique_ptr<SphereGizmo> _sphere;
    std::unique_ptr<ConeGizmo> _cone;
    std::unique_ptr<CapsuleGizmo> _capsule;
    std::unique_ptr<CylinderGizmo> _cylinder;
    std::array<ShapeGizmo*, 5> _gizmos{};
    size_t _shown = 0;
};

VISUTWIN_EXAMPLE_MAIN(ShapeGizmoExample)
