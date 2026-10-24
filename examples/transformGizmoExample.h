// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The scene the three transform gizmo examples share, which differ only in the gizmo
// they attach: a default white box at the origin, lit by a default directional light
// at euler (0, 0, -60) with ambient 0.2, over the default infinite Grid. The gizmo, drawn in its own
// depth-cleared layer from Gizmo::createLayer, is attached to the box; its size is
// 1024 / the canvas height, kept up to date as the window resizes, and the orbit camera
// (focused on the origin, zoom 2..10, pitch +/-89.999) stops responding while the gizmo
// holds the pointer (the 'gizmo:pointer' app event).
//
// The example harness re-enables the camera controls at the top of every frame (for its
// HUD), so the gizmo's pointer hold is re-applied in update().
//
// While dragging, the gizmo's transform readout shows below it (gizmoLabels.h).
//
#pragma once

#include <memory>
#include <string>
#include <utility>

#include "exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/gizmo/gizmo.h"
#include "framework/gizmo/transformGizmo.h"
#include "extras/grid/grid.h"
#include "scene/materials/standardMaterial.h"
#include "gizmoLabels.h"
#include "uiElements.h"

namespace visutwin::canvas
{
    template <class GizmoType>
    class TransformGizmoExample : public ExampleApp
    {
    protected:
        explicit TransformGizmoExample(std::string title): ExampleApp({.title = std::move(title)}) {}

        void configure(AppOptions& options) override
        {
            registerUi(options);
        }

        bool create() override
        {
            scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

            // A box with a default StandardMaterial.
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

            // 'gizmo:pointer' disables the camera controls while the gizmo holds
            // the pointer.
            engine()->on("gizmo:pointer", [this](const bool hasPointer) {
                _gizmoHasPointer = hasPointer;
                if (_controls) {
                    _controls->setInputBlocked(hasPointer);
                }
            });

            // Gizmo
            _gizmoLayer = Gizmo::createLayer(engine());
            _gizmo = std::make_unique<GizmoType>(camera, _gizmoLayer);
            _gizmo->on(Gizmo::EVENT_POINTERDOWN, [this](float, float, MeshInstance* meshInstance) {
                engine()->fire("gizmo:pointer", meshInstance != nullptr);
            });
            _gizmo->on(Gizmo::EVENT_POINTERUP, [this]() {
                engine()->fire("gizmo:pointer", false);
            });
            _gizmo->attach(box);
            resize();

            auto* gridEntity = new Entity();
            gridEntity->setEngine(engine());
            gridEntity->setName("grid");
            root()->addChild(gridEntity);
            _grid = std::make_unique<Grid>(device());
            _grid->attach(gridEntity);

            createReadout();
            return true;
        }

        void update(float /*dt*/) override
        {
            resize();
            if (_controls && _gizmoHasPointer) {
                _controls->setInputBlocked(true);
            }
            updateReadout();
        }

        void destroy() override
        {
            // Both own entities in the engine's hierarchy; the gizmo also leaves its layer.
            _labels.reset();
            _gizmo.reset();
            _grid.reset();
        }

    private:
        // The readout: labels on a screen measured in canvas points (scale mode none).
        void createReadout()
        {
            _fontAsset = std::make_unique<Asset>("readout-font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
            FontResource* font = _fontAsset->resourceAs<FontResource>();
            ScreenComponent* screen = createScreen({.scaleMode = ScreenScaleMode::None});
            if (!font || !screen) {
                spdlog::warn("the gizmo readout needs fonts/roboto-regular.json");
                return;
            }
            _labels = std::make_unique<GizmoLabels>(engine(), screen, font);
        }

        void updateReadout()
        {
            if (_labels && _gizmo) {
                _labels->update(_gizmo->readoutState());
            }
        }

        // Keep the gizmo size consistent to the canvas size (1024 / its height, or
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

        std::shared_ptr<StandardMaterial> _boxMaterial;
        std::shared_ptr<Layer> _gizmoLayer;
        std::unique_ptr<GizmoType> _gizmo;
        int _gizmoSizeDim = 0;
        std::unique_ptr<Grid> _grid;
        Entity* _cameraEntity = nullptr;
        CameraControls* _controls = nullptr;
        bool _gizmoHasPointer = false;
        std::unique_ptr<Asset> _fontAsset;
        std::unique_ptr<GizmoLabels> _labels;
    };
}
