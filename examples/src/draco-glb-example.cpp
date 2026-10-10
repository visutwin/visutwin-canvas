// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.10.2026
//
// Port of upstream loaders/draco-glb.
//
// Loads heart_draco.glb, a heart whose geometry is KHR_draco_mesh_compression
// compressed, scaled 20x at the origin with shadow receiving off, under an ambient
// light of 0.2. A camera at (0, 0.5, 4) clears to grey 0.2, and a default-range omni
// light of intensity 3 at (1, 1, 5) lights the heart, which turns about the world X
// and Y axes by (4, -20) degrees a second. No keys.
//
// DEVIATIONS:
// - The engine decodes Draco natively in the glTF parser, so upstream's decoder
//   module setup (a wasm module with a JS fallback) has no counterpart.
// - The back buffer renders at one pixel per point, as every example here does,
//   where upstream caps the device pixel ratio at 2.
//
#include <memory>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "scene/camera.h"

using namespace visutwin::canvas;

namespace
{
    constexpr const char* kModel = "models/heart_draco.glb";

    // Degrees a second about the world X and Y axes.
    constexpr float kSpinX = 4.0f;
    constexpr float kSpinY = -20.0f;
}

class DracoGlbExample final: public ExampleApp
{
public:
    DracoGlbExample(): ExampleApp({.title = "Draco GLB"}) {}

protected:
    bool create() override
    {
        _asset = std::make_unique<Asset>("heart", AssetType::CONTAINER, assetPath(kModel));
        ContainerResource* container = _asset->resourceAs<ContainerResource>();
        if (!container) {
            spdlog::error("Failed to load {}", kModel);
            return false;
        }

        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

        // Create an instance using render component.
        _heart = container->instantiateRenderEntity();
        if (!_heart) {
            spdlog::error("Failed to instantiate {}", kModel);
            return false;
        }
        _heart->setEngine(engine());
        for (auto* render : _heart->findComponents<RenderComponent>()) {
            render->setReceiveShadows(false);
        }
        root()->addChild(_heart);
        _heart->setLocalScale(20.0f, 20.0f, 20.0f);

        // Create an entity with a camera component. It has no parent and starts at the
        // origin, so the translation is its position.
        Entity* camera = createCamera(Vector3(0.0f, 0.5f, 4.0f));
        if (auto* component = camera->findComponent<CameraComponent>();
            component && component->camera()) {
            component->camera()->setClearColor(Color(0.2f, 0.2f, 0.2f, 1.0f));
        }

        // Create an entity with an omni light component.
        auto* light = new Entity();
        light->setEngine(engine());
        if (auto* component = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
            component->setType(LightType::LIGHTTYPE_OMNI);
            component->setIntensity(3.0f);
        }
        light->setLocalPosition(1.0f, 1.0f, 5.0f);
        root()->addChild(light);

        return true;
    }

    void update(const float dt) override
    {
        if (_heart) {
            _heart->rotate(kSpinX * dt, kSpinY * dt, 0.0f);
        }
    }

private:
    std::unique_ptr<Asset> _asset;
    Entity* _heart = nullptr;
};

VISUTWIN_EXAMPLE_MAIN(DracoGlbExample)
