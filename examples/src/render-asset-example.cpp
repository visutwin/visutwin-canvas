// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/render-asset.
//
// The marble statue at the origin, turning about the vertical axis at -12 degrees a second,
// between two copies of the cube model, each scaled 3, at (7, 12, 0) and (-7, 12, 0); the
// second is a clone of the first. The cubes tumble about all three axes, at (3, 10, 6) and
// (-7, 5, -2) degrees a second. Everything is lit by the helipad env atlas, which is also
// the skybox (mip 1). The camera at (-20, 15, 20) looks at (0, 7, 0), with far clip 100,
// ACES tone mapping and a clear colour of (0.2, 0.1, 0.1).
//
#include <array>
#include <memory>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/handlers/containerResource.h"
#include "scene/constants.h"

using namespace visutwin::canvas;

class RenderAssetExample final: public ExampleApp
{
public:
    RenderAssetExample(): ExampleApp({.title = "Render Asset"}) {}

protected:
    bool create() override
    {
        _helipadAsset = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _statueAsset = std::make_unique<Asset>("statue", AssetType::CONTAINER, assetPath("models/statue.glb"));
        _cubeAsset = std::make_unique<Asset>("cube", AssetType::CONTAINER, assetPath("models/playcanvas-cube.glb"));
        auto* helipad = _helipadAsset->resourceAs<Texture>();
        auto* statue = _statueAsset->resourceAs<ContainerResource>();
        auto* cube = _cubeAsset->resourceAs<ContainerResource>();
        if (!helipad || !statue || !cube) {
            spdlog::error("Failed to load the helipad env atlas, the statue or the cube");
            return false;
        }

        // Get the instance of the cube it set up with render component and add it to scene
        _cubeEntities[0] = cube->instantiateRenderEntity();
        _cubeEntities[0]->setEngine(engine());
        _cubeEntities[0]->setLocalPosition(7.0f, 12.0f, 0.0f);
        _cubeEntities[0]->setLocalScale(3.0f, 3.0f, 3.0f);
        root()->addChild(_cubeEntities[0]);

        // Clone another copy of it and add it to scene
        _cubeEntities[1] = _cubeEntities[0]->clone();
        _cubeEntities[1]->setLocalPosition(-7.0f, 12.0f, 0.0f);
        _cubeEntities[1]->setLocalScale(3.0f, 3.0f, 3.0f);
        root()->addChild(_cubeEntities[1]);

        // Get the instance of the statue and set up with render component
        _statueEntity = statue->instantiateRenderEntity();
        _statueEntity->setEngine(engine());
        root()->addChild(_statueEntity);

        // Create an Entity with a camera component
        auto* camera = createCamera(Vector3(-20.0f, 15.0f, 20.0f));
        camera->lookAt(0.0f, 7.0f, 0.0f);
        auto* cameraComponent = camera->findComponent<CameraComponent>();
        if (!cameraComponent || !cameraComponent->camera()) {
            spdlog::error("Failed to create the camera");
            return false;
        }
        cameraComponent->camera()->setClearColor(Color(0.2f, 0.1f, 0.1f, 1.0f));
        cameraComponent->camera()->setFarClip(100.0f);
        cameraComponent->setToneMapping(TONEMAP_ACES);

        // Set skybox
        scene()->setEnvAtlas(helipad);
        scene()->setSkyboxMip(1);

        return true;
    }

    void update(const float dt) override
    {
        // Spin the meshes
        if (_cubeEntities[0]) {
            _cubeEntities[0]->rotate(3.0f * dt, 10.0f * dt, 6.0f * dt);
        }
        if (_cubeEntities[1]) {
            _cubeEntities[1]->rotate(-7.0f * dt, 5.0f * dt, -2.0f * dt);
        }
        if (_statueEntity) {
            _statueEntity->rotate(0.0f, -12.0f * dt, 0.0f);
        }
    }

    void destroy() override
    {
        // The scene borrows the env atlas; it is torn down after this
        if (scene()) {
            scene()->setEnvAtlas(nullptr);
        }
    }

private:
    std::unique_ptr<Asset> _helipadAsset;
    std::unique_ptr<Asset> _statueAsset;
    std::unique_ptr<Asset> _cubeAsset;
    std::array<Entity*, 2> _cubeEntities = {nullptr, nullptr};
    Entity* _statueEntity = nullptr;
};

VISUTWIN_EXAMPLE_MAIN(RenderAssetExample)
