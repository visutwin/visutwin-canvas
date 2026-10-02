// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream gizmos/transform-rotate.
//
// A default white box at the origin, lit by a default directional light at euler
// (0, 0, -60) with ambient 0.2, over a 4x4 grid. A rotate gizmo, drawn in its own
// depth-cleared layer from Gizmo::createLayer, is attached to the box; its size is
// 1024 / the canvas height, kept up to date as the window resizes, and the orbit camera
// (focused on the origin, zoom 2..10, pitch +/-89.999) stops responding while the gizmo
// holds the pointer (upstream's 'gizmo:pointer' app event).
//
// DEVIATIONS:
// - No controls panel: upstream's initial values apply (snap off, world space, rotation
//   mode 'absolute', the default theme, drag mode 'selected', perspective at 45
//   degrees).
// - Upstream's Grid script is a pristine-grid shader on a blended plane. It is drawn
//   here with a WideLineRenderer: opaque 1-pixel lines every unit over the scaled
//   (4, 1, 4) extent in upstream's 0.7 grey, with the axis lines in its colorX and
//   colorZ. The anti-aliased coverage alpha and the 0.1-unit HIGH resolution level
//   are not reproduced, so the lines read brighter than upstream's.
// - The example harness re-enables the camera controls at the top of every frame (for
//   its HUD), so the gizmo's pointer hold is re-applied in update().
//
#include <memory>
#include <vector>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/gizmo/rotateGizmo.h"
#include "scene/graphics/wideLine.h"
#include "scene/graphics/wideLineRenderer.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class TransformRotateExample final: public ExampleApp
{
public:
    TransformRotateExample(): ExampleApp({.title = "Transform Rotate"}) {}

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
            _controls->setRotateDamping(0.95f);
            _controls->setMoveDamping(0.95f);
            _controls->setZoomDamping(0.95f);
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

        // Upstream: 'gizmo:pointer' disables the camera controls while the gizmo holds
        // the pointer.
        engine()->on("gizmo:pointer", [this](const bool hasPointer) {
            _gizmoHasPointer = hasPointer;
            if (_controls) {
                _controls->setInputBlocked(hasPointer);
            }
        });

        // Gizmo
        _gizmoLayer = Gizmo::createLayer(engine());
        _gizmo = std::make_unique<RotateGizmo>(camera, _gizmoLayer);
        _gizmo->on(Gizmo::EVENT_POINTERDOWN, [this](float, float, MeshInstance* meshInstance) {
            engine()->fire("gizmo:pointer", meshInstance != nullptr);
        });
        _gizmo->on(Gizmo::EVENT_POINTERUP, [this]() {
            engine()->fire("gizmo:pointer", false);
        });
        _gizmo->attach(box);
        resize();

        createGrid(4.0f, 4.0f);
        return true;
    }

    void update(float) override
    {
        resize();
        if (_controls && _gizmoHasPointer) {
            _controls->setInputBlocked(true);
        }
        if (_gridRenderer) {
            _gridRenderer->update();
        }
    }

    void destroy() override
    {
        // Both own entities in the engine's hierarchy; the gizmo also leaves its layer.
        _gizmo.reset();
        _gridRenderer.reset();
    }

private:
    // Upstream: keep the gizmo size consistent to the canvas size (1024 / its height, or
    // its width under a horizontal fov).
    void resize()
    {
        const auto [width, height] = engine()->canvasSize();
        const auto* camera = _cameraEntity ? _cameraEntity->findComponent<CameraComponent>() : nullptr;
        const int dim = camera && camera->camera() && camera->camera()->horizontalFov() ? width : height;
        if (_gizmo && dim > 0 && dim != _gizmoSizeDim) {
            _gizmoSizeDim = dim;
            _gizmo->setSize(1024.0f / static_cast<float>(dim));
        }
    }

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
    std::shared_ptr<Layer> _gizmoLayer;
    std::unique_ptr<RotateGizmo> _gizmo;
    int _gizmoSizeDim = 0;
    std::unique_ptr<WideLineRenderer> _gridRenderer;
    std::vector<std::unique_ptr<WideLine>> _gridLines;
    Entity* _cameraEntity = nullptr;
    CameraControls* _controls = nullptr;
    bool _gizmoHasPointer = false;
};

VISUTWIN_EXAMPLE_MAIN(TransformRotateExample)
