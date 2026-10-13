// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream camera/fly.
//
// The statue under the helipad environment atlas (ambient 0.4, skybox mip 1 at intensity
// 0.4) and one default directional light at euler (45, 30, 0). The camera starts at
// (0, 20, 30) looking at the centre of the statue's bounding box, with CameraControls in
// fly mode only (orbit and pan off), its move speeds scaled by the statue's size: 2S, 4S
// fast and S slow, S the length of the box's half extents.
//
// WASDQE move, hold Shift to move fast, hold Ctrl to move slow, LMB / RMB fly.
//
// DEVIATION: upstream's controls panel (rotate speed and joystick sensitivity, move speeds,
// damping, pitch and yaw ranges, gamepad dead zone, mobile input layout) has no counterpart;
// the camera runs with CameraControls' defaults, which are upstream's, and the move speeds
// above. Upstream's touch joystick overlay is drawn with page elements and is not ported.
//
#include <memory>

#include "core/shape/boundingBox.h"
#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"

using namespace visutwin::canvas;

class FlyExample final: public ExampleApp
{
public:
    FlyExample(): ExampleApp({.title = "Fly"}) {}

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

        // A directional light
        auto* light = new Entity();
        light->setEngine(engine());
        light->addComponent<LightComponent>();
        light->setLocalEulerAngles(45.0f, 30.0f, 0.0f);
        root()->addChild(light);

        auto* statue = statueContainer->instantiateRenderEntity();
        statue->setLocalPosition(0.0f, -0.5f, 0.0f);
        root()->addChild(statue);

        const BoundingBox bbox = entityBounds(statue);

        auto* camera = createCamera(Vector3(0.0f, 20.0f, 30.0f));
        CameraControls* controls = addOrbitControls(camera, bbox.center());
        if (!controls) {
            return false;
        }
        // Fly is turned on before orbit goes off, so the controls switch straight to fly
        controls->setEnableFly(true);
        controls->setEnableOrbit(false);
        controls->setEnablePan(false);

        const float sceneSize = bbox.halfExtents().length();
        controls->setMoveSpeed(2.0f * sceneSize);
        controls->setMoveFastSpeed(4.0f * sceneSize);
        controls->setMoveSlowSpeed(sceneSize);
        controls->storeResetState();

        return true;
    }

private:
    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _statue;
};

VISUTWIN_EXAMPLE_MAIN(FlyExample)
