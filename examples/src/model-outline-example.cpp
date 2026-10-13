// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/model-outline.
//
// A green 20 x 20 ground plane carries a red sphere at (-2, 1, 0), a yellow box at (2, 1, 0)
// and a cyan cone at (0, 1, -2), each scaled 2, all diffuse StandardMaterials under a 0.2
// grey ambient. A white omni light at (0, 2, 5) (range 20, shadow bias 0.05, normal offset
// bias 0.03) casts their shadows. The camera clears to (0.2, 0.2, 0.4), renders the World
// and UI layers, and circles the scene at radius 12 and height 5, one radian a second,
// always looking at the origin. The yellow box alone is outlined in (0, 0.5, 1): a second
// camera following the main one renders it into an offscreen target, from which the
// outline is drawn over the frame.
//
// DEVIATION: upstream renders the box through its own layer and camera into a render target
// and draws the outline with a legacy post effect, a 3-pixel ring outside the silhouette.
// The engine has no post effects on that path; its OutlineRenderer does the same job (its
// own layer, a camera mirroring the main one, an offscreen silhouette) but dilates the
// silhouette by its fixed separable extend and has no thickness setting, so the outline is
// thinner than upstream's and also marks the silhouette's inner edge.
//
#include <cmath>
#include <memory>
#include <vector>

#include "../exampleApp.h"
#include "framework/extras/outlineRenderer.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class ModelOutlineExample final: public ExampleApp
{
public:
    ModelOutlineExample(): ExampleApp({.title = "Model Outline"}) {}

protected:
    bool create() override
    {
        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

        // Create ground plane and 3 primitives
        createColoredPrimitive("plane", Vector3(0.0f, 0.0f, 0.0f), Vector3(20.0f, 20.0f, 20.0f),
            Color(0.3f, 0.5f, 0.3f, 1.0f));
        createColoredPrimitive("sphere", Vector3(-2.0f, 1.0f, 0.0f), Vector3(2.0f, 2.0f, 2.0f),
            Color(1.0f, 0.0f, 0.0f, 1.0f));
        Entity* box = createColoredPrimitive("box", Vector3(2.0f, 1.0f, 0.0f), Vector3(2.0f, 2.0f, 2.0f),
            Color(1.0f, 1.0f, 0.0f, 1.0f));
        createColoredPrimitive("cone", Vector3(0.0f, 1.0f, -2.0f), Vector3(2.0f, 2.0f, 2.0f),
            Color(0.0f, 1.0f, 1.0f, 1.0f));

        // Create main camera, which renders entities in world layer
        _camera = createCamera(Vector3(0.0f, 20.0f, 25.0f));
        _camera->setName("MainCamera");
        _camera->lookAt(Vector3(0.0f, 0.0f, 0.0f));
        auto* cameraComponent = _camera->findComponent<CameraComponent>();
        if (!cameraComponent || !cameraComponent->camera()) {
            spdlog::error("Failed to create the camera");
            return false;
        }
        cameraComponent->camera()->setClearColor(Color(0.2f, 0.2f, 0.4f, 1.0f));
        cameraComponent->setLayers({LAYERID_WORLD, LAYERID_UI});

        // The outline: its own layer, and a camera following the main one that renders the
        // box into an offscreen target
        _outlineRenderer = std::make_unique<OutlineRenderer>(engine());
        _outlineRenderer->addEntity(box, Color(0.0f, 0.5f, 1.0f, 1.0f));

        // Create an Entity with a omni light component
        auto* light = new Entity();
        light->setEngine(engine());
        if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
            lightComponent->setType(LightType::LIGHTTYPE_OMNI);
            lightComponent->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
            lightComponent->setRange(20.0f);
            lightComponent->setCastShadows(true);
            lightComponent->setShadowBias(0.05f);
            lightComponent->setShadowNormalBias(0.03f);
            lightComponent->setLayers({LAYERID_WORLD});
        }
        light->setLocalPosition(0.0f, 2.0f, 5.0f);
        root()->addChild(light);

        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        // Rotate the camera around the objects
        _camera->setLocalPosition(12.0f * std::sin(_time), 5.0f, 12.0f * std::cos(_time));
        _camera->lookAt(Vector3(0.0f, 0.0f, 0.0f));

        // Outline camera needs to match the main camera
        if (_outlineRenderer) {
            _outlineRenderer->frameUpdate(_camera);
        }
    }

    void destroy() override
    {
        // It owns a camera entity and renderer passes, released while the engine is alive
        _outlineRenderer.reset();
    }

private:
    Entity* createColoredPrimitive(const char* type, const Vector3& position, const Vector3& scale,
        const Color& color)
    {
        // Create material of specified color
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(color);
        _materials.push_back(material);
        return createPrimitive(type, material.get(), position, scale);
    }

    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::unique_ptr<OutlineRenderer> _outlineRenderer;
    Entity* _camera = nullptr;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(ModelOutlineExample)
