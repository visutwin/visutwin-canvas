// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream gizmos/transform-scale.
//
// A default white box at the origin, lit by a default directional light at euler
// (0, 0, -60) with ambient 0.2, over a 4x4 grid. A scale gizmo is attached to the
// box; the orbit camera (focused on the origin, zoom 2..10, pitch +/-89.999) stops
// responding while the gizmo holds the pointer.
//
// DEVIATIONS:
// - The engine's TransformGizmo is a simplified gizmo (box ends on axis lines and a centre
//   box, no plane handles, fixed world size of 1.8). Upstream's gizmo size of
//   1024 / viewport dimension, its theme, snap, coordinate-space, drag-mode,
//   uniform and per-shape render settings have no counterpart, so the controls
//   panel is absent
//   and upstream's initial values (snap off, world space) apply.
// - Upstream's Grid script is a pristine-grid shader on a blended plane. It is drawn
//   here with a WideLineRenderer: opaque 1-pixel lines every unit over the scaled
//   (4, 1, 4) extent in upstream's 0.7 grey, with the axis lines in its colorX and
//   colorZ. The anti-aliased coverage alpha and the 0.1-unit HIGH resolution level
//   are not reproduced, so the lines read brighter than upstream's.
// - CameraControls has no sceneSize or damping settings (upstream: 5 and 0.95);
//   the camera moves undamped.
// - No camera projection / FOV panel; upstream's initial perspective, 45 degrees.
//
#include <memory>
#include <vector>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/gizmo/transformGizmo.h"
#include "scene/graphics/wideLine.h"
#include "scene/graphics/wideLineRenderer.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class TransformScaleExample final: public ExampleApp
{
public:
    TransformScaleExample(): ExampleApp({.title = "Transform Scale"}) {}

protected:
    bool create() override
    {
        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

        // A box with the default material: upstream's is a default StandardMaterial.
        _boxMaterial = std::make_shared<StandardMaterial>();
        auto* box = createPrimitive("box", _boxMaterial.get());

        // Camera
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

        // Camera controls
        _controls = addOrbitControls(_cameraEntity, Vector3(0.0f, 0.0f, 0.0f));
        if (_controls) {
            _controls->setPitchRange(Vector2(-89.999f, 89.999f));
            _controls->setZoomRange(Vector2(2.0f, 10.0f));
            _controls->setEnableFly(false);
            _controls->storeResetState();
        }

        // Light: a default light component is directional, white, intensity 1.
        auto* light = new Entity();
        light->setEngine(engine());
        light->addComponent<LightComponent>();
        root()->addChild(light);
        light->setLocalEulerAngles(0.0f, 0.0f, -60.0f);

        // Gizmo
        _gizmo = std::make_unique<TransformGizmo>(engine(), camera);
        _gizmo->setMode(TransformGizmo::Mode::Scale);
        _gizmo->attach(box);

        createGrid(4.0f, 4.0f);
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (!_gizmo) {
            return false;
        }

        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window(), &width, &height);
        const bool consumed = _gizmo->handleEvent(event, width, height);

        // Upstream: gizmo 'pointer:down' on a handle disables the camera controls,
        // 'pointer:up' re-enables them.
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && consumed) {
            _gizmoHasPointer = true;
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            _gizmoHasPointer = false;
        }
        if (_controls) {
            _controls->setInputBlocked(_gizmoHasPointer);
        }
        return consumed;
    }

    void update(float) override
    {
        if (_gizmo) {
            _gizmo->update();
        }
        if (_gridRenderer) {
            _gridRenderer->update();
        }
    }

    void destroy() override
    {
        // Both own entities in the engine's hierarchy.
        _gizmo.reset();
        _gridRenderer.reset();
    }

private:
    // Upstream Grid script on an entity scaled (sx, 1, sz): half extents sx/2, sz/2,
    // unit lines in 0.7 grey, the x = 0 line in colorZ and the z = 0 line in colorX.
    void createGrid(const float scaleX, const float scaleZ)
    {
        _gridRenderer = std::make_unique<WideLineRenderer>(engine(), device());
        _gridRenderer->setScreenSize(static_cast<float>(windowWidth()), static_cast<float>(windowHeight()));

        const Color lineColor(0.7f, 0.7f, 0.7f, 1.0f);
        const Color colorX(1.0f, 0.3f, 0.3f, 1.0f);
        const Color colorZ(0.3f, 0.3f, 1.0f, 1.0f);
        const float hx = scaleX * 0.5f;
        const float hz = scaleZ * 0.5f;

        for (int i = static_cast<int>(-hx); i <= static_cast<int>(hx); ++i) {
            auto& line = _gridLines.emplace_back(std::make_unique<WideLine>());
            const auto x = static_cast<float>(i);
            line->setPoints({Vector3(x, 0.0f, -hz), Vector3(x, 0.0f, hz)}, i == 0 ? colorZ : lineColor, 1.0f);
        }
        for (int i = static_cast<int>(-hz); i <= static_cast<int>(hz); ++i) {
            auto& line = _gridLines.emplace_back(std::make_unique<WideLine>());
            const auto z = static_cast<float>(i);
            line->setPoints({Vector3(-hx, 0.0f, z), Vector3(hx, 0.0f, z)}, i == 0 ? colorX : lineColor, 1.0f);
        }
        for (auto& line : _gridLines) {
            _gridRenderer->add(line.get());
        }
    }

    std::shared_ptr<StandardMaterial> _boxMaterial;
    std::unique_ptr<TransformGizmo> _gizmo;
    std::unique_ptr<WideLineRenderer> _gridRenderer;
    std::vector<std::unique_ptr<WideLine>> _gridLines;
    Entity* _cameraEntity = nullptr;
    CameraControls* _controls = nullptr;
    bool _gizmoHasPointer = false;
};

VISUTWIN_EXAMPLE_MAIN(TransformScaleExample)
