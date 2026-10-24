// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// Port of upstream gizmos/light.
//
// A LightGizmo on a warm spot light (colour 1, 0.85, 0.6, range 4, cone 25 / 40 degrees) above and
// to the side of a default box, aimed at it and scaled (1, 1, 0.5) to size its area shape, in a
// layer that keeps the scene depth; area lights are on, so the light can take a shape. Its
// handles drag the range, the cone angles and the area size; the camera stops orbiting while one
// is dragged (zoom 1..20).
//
// Keys stand in for upstream's controls panel:
//   T      light type, cycled spot / omni / directional
//   K      light shape, cycled punctual / rectangle / disk / sphere
//   G      ground line on / off
//   C      draw in the light's colour on / off
//   H      handles on / off
//   R      readout on / off
//   I      icon on / off
//   S      snap on / off (lengths by 1, angles by 5 degrees)
//   [ / ]  line thickness -/+ 0.5 (0.5..8)
//   P      projection, perspective / orthographic
// VISUTWIN_GIZMO_FILL=a sets the fill colour's alpha (0.05), for a capture that shows the volume.
//
// DEVIATIONS:
// - No controls panel: keys toggle the settings, and the theme keeps its defaults.
// - Upstream loads the area light LUTs from a JSON asset; this engine has them built in.
// - The camera controls have no `sceneSize` here; with fly off it changes nothing upstream shows.
//
#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>

#include "../overlayGizmoExample.h"
#include "framework/gizmo/lightGizmo.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr std::array<LightType, 3> kTypes = {LightType::LIGHTTYPE_SPOT, LightType::LIGHTTYPE_OMNI,
        LightType::LIGHTTYPE_DIRECTIONAL};
    constexpr std::array<LightShape, 4> kShapes = {LightShape::LIGHTSHAPE_PUNCTUAL, LightShape::LIGHTSHAPE_RECT,
        LightShape::LIGHTSHAPE_DISK, LightShape::LIGHTSHAPE_SPHERE};
}

class LightGizmoExample final : public OverlayGizmoExample
{
public:
    LightGizmoExample() : OverlayGizmoExample("Light Gizmo") {}

protected:
    bool create() override
    {
        if (!createStage(4.0f, Vector2(1.0f, 20.0f))) {
            return false;
        }
        scene()->lighting().areaLightsEnabled = true;

        _boxMaterial = std::make_shared<StandardMaterial>();
        createPrimitive("box", _boxMaterial.get());

        // the light above and to the side of the box, scaled to size its area shape
        _light = new Entity();
        _light->setEngine(engine());
        _light->setName("light");
        _lightComponent = static_cast<LightComponent*>(_light->addComponent<LightComponent>());
        _lightComponent->setType(LightType::LIGHTTYPE_SPOT);
        _lightComponent->setColor(Color(1.0f, 0.85f, 0.6f, 1.0f));
        _lightComponent->setRange(4.0f);
        _lightComponent->setInnerConeAngle(25.0f);
        _lightComponent->setOuterConeAngle(40.0f);
        root()->addChild(_light);
        _light->setPosition(Vector3(-1.5f, 2.5f, 1.5f));
        _light->setLocalScale(1.0f, 1.0f, 0.5f);

        // aim the light at the box: lookAt points the forward axis, and a light shines along -y
        _light->lookAt(Vector3(0.0f));
        _light->rotateLocal(90.0f, 0.0f, 0.0f);

        _gizmo = std::make_unique<LightGizmo>(_camera, _layer);
        watch(_gizmo.get());
        _gizmo->attach(_light);
        if (const char* fill = std::getenv("VISUTWIN_GIZMO_FILL")) {
            Color color = _gizmo->theme().fill;
            color.a = static_cast<float>(std::atof(fill));
            _gizmo->setTheme({.fill = color});
        }

        spdlog::info("Keys: T type | K shape | G ground | C light colour | H handles | R readout | I icon | "
                     "S snap | [ ] line thickness | P projection");
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
        if (!keyboard || !_gizmo) {
            return;
        }
        if (keyboard->wasPressed(Key::T)) {
            _type = (_type + 1) % kTypes.size();
            _lightComponent->setType(kTypes[_type]);
        }
        if (keyboard->wasPressed(Key::K)) {
            _shape = (_shape + 1) % kShapes.size();
            _lightComponent->setShape(kShapes[_shape]);
        }
        if (keyboard->wasPressed(Key::G)) _gizmo->groundLine = !_gizmo->groundLine;
        if (keyboard->wasPressed(Key::C)) _gizmo->useLightColor = !_gizmo->useLightColor;
        if (keyboard->wasPressed(Key::H)) _gizmo->handles = !_gizmo->handles;
        if (keyboard->wasPressed(Key::R)) _gizmo->readout = !_gizmo->readout;
        if (keyboard->wasPressed(Key::I)) _gizmo->icon = !_gizmo->icon;
        if (keyboard->wasPressed(Key::S)) _gizmo->snap = !_gizmo->snap;
        if (keyboard->wasPressed(Key::LeftBracket)) {
            _gizmo->setLineThickness(std::max(_gizmo->lineThickness() - 0.5f, 0.5f));
        }
        if (keyboard->wasPressed(Key::RightBracket)) {
            _gizmo->setLineThickness(std::min(_gizmo->lineThickness() + 0.5f, 8.0f));
        }
        if (keyboard->wasPressed(Key::P)) toggleProjection();
    }

    std::shared_ptr<StandardMaterial> _boxMaterial;
    std::unique_ptr<LightGizmo> _gizmo;
    Entity* _light = nullptr;
    LightComponent* _lightComponent = nullptr;
    size_t _type = 0;
    size_t _shape = 0;
};

VISUTWIN_EXAMPLE_MAIN(LightGizmoExample)
