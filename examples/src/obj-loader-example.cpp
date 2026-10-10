// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.10.2026
//
// Port of upstream loaders/obj.
//
// Loads monkey.obj, three Suzanne heads (one OBJ object each), as a container and
// gives every mesh instance its own StandardMaterial with a random diffuse colour.
// The model spins about the world Y axis at 100 degrees a second, lit by a white omni
// light (range 100) at (5, 0, 15) and a 0.2 grey ambient, seen by a camera at
// (0, 0, 5) clearing to (0.4, 0.45, 0.5). No keys.
//
// DEVIATION: the random colours come from a fixed seed, so every run (and a
// screenshot under VISUTWIN_FIXED_DT) draws the same colours; upstream picks new ones
// from Math.random() on every load.
//
// DEVIATION: the OBJ goes through the engine's own parser, which splits the file into
// one mesh instance per OBJ object, where upstream's sample parser starts a mesh
// instance at every `o`, `g` and `usemtl` line. In this file each object is followed
// at once by its single `usemtl`, so both give the same three mesh instances. The
// file's `mtllib monkey.mtl` names a file that does not exist (upstream ships none
// either); the parser warns and falls back to its default material, which the random
// materials replace.
//
#include <memory>
#include <random>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr const char* kModel = "models/monkey.obj";

    // Degrees per second about the world Y axis.
    constexpr float kSpinSpeed = 100.0f;
}

class ObjLoaderExample final: public ExampleApp
{
public:
    ObjLoaderExample(): ExampleApp({.title = "OBJ"}) {}

protected:
    bool create() override
    {
        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

        _asset = std::make_unique<Asset>("monkey", AssetType::CONTAINER, assetPath(kModel));
        ContainerResource* container = _asset->resourceAs<ContainerResource>();
        if (!container) {
            spdlog::error("Failed to load {}", kModel);
            return false;
        }

        _model = container->instantiateRenderEntity();
        if (!_model) {
            spdlog::error("Failed to instantiate {}", kModel);
            return false;
        }
        _model->setEngine(engine());
        root()->addChild(_model);

        // Add a randomly generated material to all mesh instances.
        std::mt19937 rng(1u);
        std::uniform_real_distribution<float> random01(0.0f, 1.0f);
        int meshInstanceCount = 0;
        for (auto* render : _model->findComponents<RenderComponent>()) {
            for (auto* meshInstance : render->meshInstances()) {
                auto material = std::make_shared<StandardMaterial>();
                const float r = random01(rng);
                const float g = random01(rng);
                const float b = random01(rng);
                material->setDiffuse(Color(r, g, b));
                meshInstance->setMaterial(material);
                ++meshInstanceCount;
            }
        }

        // Create an entity with a camera component.
        auto* camera = createCamera(Vector3(0.0f, 0.0f, 5.0f));
        if (auto* cameraComponent = camera->findComponent<CameraComponent>()) {
            cameraComponent->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
        }

        // Create an entity with an omni light component.
        auto* light = new Entity();
        light->setEngine(engine());
        if (auto* lc = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
            lc->setType(LightType::LIGHTTYPE_OMNI);
            lc->setColor(Color(1.0f, 1.0f, 1.0f));
            lc->setRange(100.0f);
        }
        light->setLocalPosition(5.0f, 0.0f, 15.0f);
        root()->addChild(light);

        spdlog::info("OBJ: {} mesh instances", meshInstanceCount);
        return true;
    }

    void update(const float dt) override
    {
        _model->rotate(0.0f, kSpinSpeed * dt, 0.0f);
    }

private:
    std::unique_ptr<Asset> _asset;
    Entity* _model = nullptr;
};

VISUTWIN_EXAMPLE_MAIN(ObjLoaderExample)
