// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// The stage the overlay gizmo examples (camera, light, shape, icon, bounding box) share: ambient
// 0.2, a camera clearing to 0.1 grey with a far clip of 1000, starting at a multiple of its aspect
// on each axis and orbiting the origin (damping 0.95, pitch +/-89.999, no fly), a default
// directional light at euler (0, 0, -60), the default infinite Grid, a gizmo layer that keeps the
// scene depth, and the gizmos' labels (gizmoLabels.h). The camera stops orbiting while a gizmo
// holds the pointer; the example harness re-enables the controls at the top of every frame, so
// the hold is re-applied in updateStage().
//
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "exampleApp.h"
#include "extras/grid/grid.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/gizmo/gizmo.h"
#include "gizmoLabels.h"

namespace visutwin::canvas
{
    class OverlayGizmoExample : public ExampleApp
    {
    protected:
        explicit OverlayGizmoExample(std::string title) : ExampleApp({.title = std::move(title)}) {}

        void configure(AppOptions& options) override
        {
            registerUi(options);
        }

        /// The stage, the camera `cameraFactor` x its aspect out on each axis, zooming in `zoomRange`.
        bool createStage(const float cameraFactor, const Vector2& zoomRange, const std::string& layerName = "Helpers")
        {
            scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

            _cameraEntity = createCamera(Vector3(0.0f, 0.0f, 0.0f));
            _camera = _cameraEntity->findComponent<CameraComponent>();
            if (!_camera || !_camera->camera()) {
                spdlog::error("Failed to create camera");
                return false;
            }
            _camera->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
            _camera->camera()->setFarClip(1000.0f);
            const float offset = cameraFactor * _camera->camera()->aspectRatio();
            _cameraEntity->setPosition(Vector3(offset, offset, offset));

            _controls = addOrbitControls(_cameraEntity, Vector3(0.0f, 0.0f, 0.0f));
            if (_controls) {
                _controls->setRotateDamping(0.95f);
                _controls->setMoveDamping(0.95f);
                _controls->setZoomDamping(0.95f);
                _controls->setPitchRange(Vector2(-89.999f, 89.999f));
                _controls->setZoomRange(zoomRange);
                _controls->setEnableFly(false);
                _controls->storeResetState();
            }

            auto* light = new Entity();
            light->setEngine(engine());
            light->setName("light");
            light->addComponent<LightComponent>();
            root()->addChild(light);
            light->setLocalEulerAngles(0.0f, 0.0f, -60.0f);

            // the gizmo layer keeps the scene depth, so occluded parts draw faint
            _layer = Gizmo::createLayer(engine(), layerName, INT_MAX, true);

            auto* gridEntity = new Entity();
            gridEntity->setEngine(engine());
            gridEntity->setName("grid");
            root()->addChild(gridEntity);
            _grid = std::make_unique<Grid>(device());
            _grid->attach(gridEntity);

            _fontAsset = std::make_unique<Asset>("label-font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
            FontResource* font = _fontAsset->resourceAs<FontResource>();
            _screen = createScreen({.scaleMode = ScreenScaleMode::None});
            if (font && _screen) {
                _labels = std::make_unique<GizmoLabels>(engine(), _screen, font);
            } else {
                spdlog::warn("the gizmo labels need fonts/roboto-regular.json");
            }
            return true;
        }

        /// Stops the camera orbiting while `gizmo` holds the pointer.
        void watch(Gizmo* gizmo)
        {
            gizmo->on(Gizmo::EVENT_POINTERDOWN, [this](float, float, MeshInstance* meshInstance) {
                _gizmoHasPointer = meshInstance != nullptr;
                if (_controls) {
                    _controls->setInputBlocked(_gizmoHasPointer);
                }
            });
            gizmo->on(Gizmo::EVENT_POINTERUP, [this]() {
                _gizmoHasPointer = false;
                if (_controls) {
                    _controls->setInputBlocked(false);
                }
            });
        }

        /// Call once a frame with the labels of every gizmo.
        void updateStage(const std::vector<GizmoReadout>& labels)
        {
            if (_controls && _gizmoHasPointer) {
                _controls->setInputBlocked(true);
            }
            if (_labels) {
                _labels->update(labels);
            }
        }

        void toggleProjection()
        {
            Camera* camera = _camera ? _camera->camera() : nullptr;
            if (camera) {
                const bool ortho = camera->projection() == ProjectionType::Perspective;
                camera->setProjection(ortho ? ProjectionType::Orthographic : ProjectionType::Perspective);
                // An orthographic view has no depth cue, so keep what lies behind the camera visible.
                camera->setNearClip(ortho ? -camera->farClip() : 0.1f);
            }
        }

        void destroyStage()
        {
            _labels.reset();
            _grid.reset();
        }

        Entity* _cameraEntity = nullptr;
        CameraComponent* _camera = nullptr;
        CameraControls* _controls = nullptr;
        std::shared_ptr<Layer> _layer;
        ScreenComponent* _screen = nullptr;

    private:
        std::unique_ptr<Grid> _grid;
        std::unique_ptr<Asset> _fontAsset;
        std::unique_ptr<GizmoLabels> _labels;
        bool _gizmoHasPointer = false;
    };
}
