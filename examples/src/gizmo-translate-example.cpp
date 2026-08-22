// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// TransformGizmo demo: an orange box on a dark ground slab, manipulated by the
// translate/rotate/scale gizmo. Right-drag orbits the camera, the wheel zooms,
// and 1/2/3 switch gizmo mode. This example opens in translate mode.
//
#include <algorithm>
#include <cmath>
#include <memory>

#include "../exampleApp.h"
#include "framework/gizmo/transformGizmo.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class GizmoTranslateExample final: public ExampleApp
{
public:
    GizmoTranslateExample(): ExampleApp({.title = "Transform Gizmo (Translate/Rotate/Scale)", .width = 1200, .height = 760}) {}

protected:
    bool create() override
    {
        scene()->setAmbientLight(0.25f, 0.25f, 0.25f);

        _gridMaterial = std::make_shared<StandardMaterial>();
        _gridMaterial->setDiffuse(Color(0.2f, 0.22f, 0.25f, 1.0f));

        _boxMaterial = std::make_shared<StandardMaterial>();
        _boxMaterial->setDiffuse(Color(0.82f, 0.48f, 0.16f, 1.0f));

        createPrimitive("box", _gridMaterial.get(), Vector3(0.0f, -0.05f, 0.0f),
            Vector3(8.0f, 0.1f, 8.0f));
        auto* box = createPrimitive("box", _boxMaterial.get(), Vector3(0.0f, 0.5f, 0.0f),
            Vector3(1.0f, 1.0f, 1.0f));

        auto* lightEntity = new Entity();
        lightEntity->setEngine(engine());
        if (auto* light = static_cast<LightComponent*>(lightEntity->addComponent<LightComponent>())) {
            light->setType(LightType::LIGHTTYPE_DIRECTIONAL);
            light->setIntensity(2.4f);
        }
        lightEntity->setLocalEulerAngles(45.0f, 35.0f, 0.0f);
        root()->addChild(lightEntity);

        _cameraEntity = createCamera(Vector3(4.2f, 4.2f, 4.2f));
        auto* camera = _cameraEntity->findComponent<CameraComponent>();
        if (!camera || !camera->camera()) {
            spdlog::error("Failed to create camera");
            return false;
        }
        camera->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
        _cameraEntity->lookAt(_focusPoint);

        _gizmo = std::make_unique<TransformGizmo>(engine(), camera);
        _gizmo->attach(box);
        _gizmo->setMode(TransformGizmo::Mode::Translate);
        _gizmo->setSnap(false);

        spdlog::info("Controls: 1=Translate, 2=Rotate, 3=Scale, S=Toggle Snap, [ / ] adjust snap increment");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type == SDL_EVENT_KEY_DOWN) {
            switch (event.key.key) {
            case SDLK_1:
                _gizmo->setMode(TransformGizmo::Mode::Translate);
                spdlog::info("Gizmo mode: Translate");
                break;
            case SDLK_2:
                _gizmo->setMode(TransformGizmo::Mode::Rotate);
                spdlog::info("Gizmo mode: Rotate");
                break;
            case SDLK_3:
                _gizmo->setMode(TransformGizmo::Mode::Scale);
                spdlog::info("Gizmo mode: Scale");
                break;
            case SDLK_S:
                _gizmo->setSnap(!_gizmo->snap());
                spdlog::info("Snap: {}", _gizmo->snap() ? "ON" : "OFF");
                break;
            case SDLK_LEFTBRACKET:
                setSnapIncrement(std::max(0.05f, _snapIncrement - 0.05f));
                break;
            case SDLK_RIGHTBRACKET:
                setSnapIncrement(std::min(5.0f, _snapIncrement + 0.05f));
                break;
            default:
                break;
            }
        }

        // The gizmo needs the drawable size to unproject pointer positions.
        int windowWidth = 0;
        int windowHeight = 0;
        SDL_GetWindowSize(window(), &windowWidth, &windowHeight);
        if (_gizmo->handleEvent(event, windowWidth, windowHeight)) {
            return true;
        }

        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT) {
            _orbiting = true;
            _prevMouseX = event.button.x;
            _prevMouseY = event.button.y;
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_RIGHT) {
            _orbiting = false;
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION && _orbiting) {
            const float dx = event.motion.x - _prevMouseX;
            const float dy = event.motion.y - _prevMouseY;
            _prevMouseX = event.motion.x;
            _prevMouseY = event.motion.y;

            _orbitYaw -= dx * 0.25f;
            _orbitPitch = std::clamp(_orbitPitch - dy * 0.25f, -85.0f, 85.0f);
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            _orbitDist = std::clamp(_orbitDist - event.wheel.y * 0.35f, 2.0f, 20.0f);
            return true;
        }
        return false;
    }

    void update(float) override
    {
        const float pitchRad = _orbitPitch * DEG_TO_RAD;
        const float yawRad = _orbitYaw * DEG_TO_RAD;
        const Vector3 camPos(
            _focusPoint.getX() - std::sin(yawRad) * std::cos(pitchRad) * _orbitDist,
            _focusPoint.getY() + std::sin(pitchRad) * _orbitDist,
            _focusPoint.getZ() - std::cos(yawRad) * std::cos(pitchRad) * _orbitDist
        );
        _cameraEntity->setPosition(camPos);
        _cameraEntity->lookAt(_focusPoint);

        _gizmo->update();
    }

    void destroy() override
    {
        // The gizmo owns entities in the engine's hierarchy.
        _gizmo.reset();
    }

private:
    void setSnapIncrement(const float increment)
    {
        _snapIncrement = increment;
        _gizmo->setTranslateSnapIncrement(_snapIncrement);
        _gizmo->setScaleSnapIncrement(std::max(0.01f, _snapIncrement * 0.2f));
        _gizmo->setRotateSnapIncrement(std::max(1.0f, _snapIncrement * 20.0f));
        spdlog::info("Snap increment: {:.2f}", _snapIncrement);
    }

    std::shared_ptr<StandardMaterial> _gridMaterial;
    std::shared_ptr<StandardMaterial> _boxMaterial;
    std::unique_ptr<TransformGizmo> _gizmo;
    Entity* _cameraEntity = nullptr;

    const Vector3 _focusPoint{0.0f, 0.5f, 0.0f};
    float _orbitYaw = 45.0f;
    float _orbitPitch = 25.0f;
    float _orbitDist = 6.0f;
    bool _orbiting = false;
    float _prevMouseX = 0.0f;
    float _prevMouseY = 0.0f;
    float _snapIncrement = 0.5f;
};

VISUTWIN_EXAMPLE_MAIN(GizmoTranslateExample)
