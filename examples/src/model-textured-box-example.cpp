// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/model-textured-box.
//
// A box with the clouds texture as its diffuse map, tumbling about all three axes, lit by a red
// omni light (range 10) circling it at a radius of 3. The light's entity also carries a small
// sphere, scaled to 0.1, drawn with the default material, which marks where the light is. An
// ambient of 0.2; the camera is fixed 10 units along +Z.
//
#include <cmath>
#include <memory>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class ModelTexturedBoxExample final: public ExampleApp
{
public:
    ModelTexturedBoxExample(): ExampleApp({.title = "Model Textured Box"}) {}

protected:
    bool create() override
    {
        _clouds = std::make_unique<Asset>("clouds", AssetType::TEXTURE, assetPath("textures/clouds.jpg"));
        Texture* clouds = _clouds->resourceAs<Texture>();
        if (!clouds) {
            spdlog::error("Failed to load textures/clouds.jpg");
            return false;
        }

        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

        // The material with the diffuse texture
        _material = std::make_shared<StandardMaterial>();
        _material->setDiffuseMap(clouds);

        // The box
        _box = createPrimitive("box", _material.get());

        // An omni light, and a sphere with no material of its own marking where it is
        _light = new Entity();
        _light->setEngine(engine());
        if (auto* component = static_cast<LightComponent*>(_light->addComponent<LightComponent>())) {
            component->setType(LightType::LIGHTTYPE_OMNI);
            component->setColor(Color(1.0f, 0.0f, 0.0f, 1.0f));
            component->setRange(10.0f);
        }
        if (auto* render = static_cast<RenderComponent*>(_light->addComponent<RenderComponent>())) {
            render->setType("sphere");
        }
        // Scale the sphere down to 0.1m
        _light->setLocalScale(0.1f, 0.1f, 0.1f);
        root()->addChild(_light);

        // The camera, 10m along the z-axis
        auto* camera = createCamera(Vector3(0.0f, 0.0f, 10.0f));
        if (auto* component = camera->findComponent<CameraComponent>(); component && component->camera()) {
            component->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
        }

        return true;
    }

    void update(const float dt) override
    {
        _angle += dt;
        if (_angle > 360.0f) {
            _angle = 0.0f;
        }

        // Move the light in a circle
        _light->setLocalPosition(3.0f * std::sin(_angle), 0.0f, 3.0f * std::cos(_angle));

        // Rotate the box
        _box->setLocalEulerAngles(_angle * 2.0f, _angle * 4.0f, _angle * 8.0f);
    }

private:
    std::unique_ptr<Asset> _clouds;
    std::shared_ptr<StandardMaterial> _material;
    Entity* _box = nullptr;
    Entity* _light = nullptr;
    float _angle = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(ModelTexturedBoxExample)
