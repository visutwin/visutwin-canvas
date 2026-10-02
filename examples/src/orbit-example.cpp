// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// Port of upstream camera/orbit.
//
// The statue under the helipad environment atlas (ambient 0.4, skybox mip 1 at
// intensity 0.4) and one default directional light at euler (45, 30, 0). The
// camera starts at (0, 20, 30) with orbit controls focused on the centre of the
// statue's bounding box, fly mode off, and move speeds scaled by the scene size.
//
// LMB / RMB orbit, Shift / MMB pan, Wheel / Pinch zoom, F focus, L look, R reset.
//
// DEVIATION: upstream's controls panel (rotate/move/zoom speeds, damping, pitch,
// yaw and zoom ranges, zoom scale min) has no counterpart; the camera runs with
// CameraControls' defaults, which are upstream's.
//
#include <memory>

#include <core/shape/boundingBox.h>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"

using namespace visutwin::canvas;

class OrbitExample final: public ExampleApp
{
public:
    OrbitExample(): ExampleApp({.title = "Orbit"}) {}

protected:
    bool create() override
    {
        _helipad = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        _statue = std::make_unique<Asset>(
            "statue", AssetType::CONTAINER, assetPath("models/statue.glb"));

        Texture* helipadTexture = _helipad->resourceAs<Texture>();
        if (!helipadTexture) {
            spdlog::error("Failed to load helipad texture");
            return false;
        }
        ContainerResource* statueContainer = _statue->resourceAs<ContainerResource>();
        if (!statueContainer) {
            spdlog::error("Failed to load statue model");
            return false;
        }

        scene()->setAmbientLight(0.4f, 0.4f, 0.4f);
        scene()->setSkyboxMip(1);
        scene()->setSkyboxIntensity(0.4f);
        scene()->setEnvAtlas(helipadTexture);

        // A directional light.
        auto* light = new Entity();
        light->setEngine(engine());
        light->addComponent<LightComponent>();
        light->setLocalEulerAngles(45.0f, 30.0f, 0.0f);
        root()->addChild(light);

        auto* statue = statueContainer->instantiateRenderEntity();
        statue->setLocalPosition(0.0f, -0.5f, 0.0f);
        root()->addChild(statue);

        const BoundingBox bbox = entityBounds(statue);
        _focusPoint = bbox.center();

        _camera = createCamera(_start);
        _controls = addOrbitControls(_camera, _focusPoint);
        if (!_controls) {
            return false;
        }
        const float sceneSize = bbox.halfExtents().length();
        _controls->setEnableFly(false);
        _controls->setMoveSpeed(2.0f * sceneSize);
        _controls->setMoveFastSpeed(4.0f * sceneSize);
        _controls->setMoveSlowSpeed(sceneSize);

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_controls) {
            return false;
        }

        switch (event.key.key) {
        case SDLK_F:
            // Keep the view direction, re-centre on the statue at the start distance.
            _controls->focus(_focusPoint, true);
            return true;
        case SDLK_L:
            // Keep the camera where it is and turn it towards the statue.
            _controls->look(_focusPoint);
            return true;
        case SDLK_R:
            // Back to the start position, looking at the statue.
            _controls->reset(_focusPoint, _start);
            return true;
        default:
            return false;
        }
    }

private:
    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _statue;

    Entity* _camera = nullptr;
    CameraControls* _controls = nullptr;
    const Vector3 _start{0.0f, 20.0f, 30.0f};
    Vector3 _focusPoint;
};

VISUTWIN_EXAMPLE_MAIN(OrbitExample)
