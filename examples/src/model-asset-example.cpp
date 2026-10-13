// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/model-asset.
//
// The marble statue, casting shadows, turns about the vertical axis at 10 degrees a second,
// beside a clone of it scaled 0.2 at (-4, 12, 0) that stands still. A white omni light at
// (5, 0, 15) (range 100) casts their shadows under a 0.2 grey ambient. The camera sits at
// (0, 7, 24) looking down -Z and clears to (0.4, 0.45, 0.5).
//
// DEVIATION: upstream instantiates the container as a legacy model entity (one model
// component holding every mesh instance); the engine has no model component, so this port
// instantiates the container's render entity hierarchy instead. The two draw the same.
//
#include <memory>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/handlers/containerResource.h"
#include "scene/constants.h"

using namespace visutwin::canvas;

class ModelAssetExample final: public ExampleApp
{
public:
    ModelAssetExample(): ExampleApp({.title = "Model Asset"}) {}

protected:
    bool create() override
    {
        _statueAsset = std::make_unique<Asset>("statue", AssetType::CONTAINER, assetPath("models/statue.glb"));
        auto* statue = _statueAsset->resourceAs<ContainerResource>();
        if (!statue) {
            spdlog::error("Failed to load models/statue.glb");
            return false;
        }

        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

        // Create an entity with render assets
        _entity = statue->instantiateRenderEntity();
        _entity->setEngine(engine());
        for (auto* render : _entity->findComponents<RenderComponent>()) {
            render->setCastShadows(true);
        }
        root()->addChild(_entity);

        // Clone a small version of the entity
        Entity* clone = _entity->clone();
        clone->setLocalScale(0.2f, 0.2f, 0.2f);
        clone->setLocalPosition(-4.0f, 12.0f, 0.0f);
        root()->addChild(clone);

        // Create an Entity with a camera component
        auto* camera = createCamera(Vector3(0.0f, 7.0f, 24.0f));
        auto* cameraComponent = camera->findComponent<CameraComponent>();
        if (!cameraComponent || !cameraComponent->camera()) {
            spdlog::error("Failed to create the camera");
            return false;
        }
        cameraComponent->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));

        // Create an Entity with a omni light component
        auto* light = new Entity();
        light->setEngine(engine());
        if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
            lightComponent->setType(LightType::LIGHTTYPE_OMNI);
            lightComponent->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
            lightComponent->setRange(100.0f);
            lightComponent->setCastShadows(true);
        }
        light->setLocalPosition(5.0f, 0.0f, 15.0f);
        root()->addChild(light);

        return true;
    }

    void update(const float dt) override
    {
        if (_entity) {
            _entity->rotate(0.0f, 10.0f * dt, 0.0f);
        }
    }

private:
    std::unique_ptr<Asset> _statueAsset;
    Entity* _entity = nullptr;
};

VISUTWIN_EXAMPLE_MAIN(ModelAssetExample)
