// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream misc/hello-world.
//
// The smallest scene there is: a unit box with a default (white) StandardMaterial at the origin,
// lit by a default directional light (white, intensity 1, no shadows) aimed by euler (45, 0, 0),
// and seen from a camera at (0, 0, 3) looking down -Z that clears to (0.5, 0.6, 0.9). The box
// turns by euler (10, 20, 30) degrees a second, in world space. No ambient light, no controls.
//
#include <memory>

#include "../exampleApp.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class HelloWorldExample final : public ExampleApp
{
public:
    HelloWorldExample() : ExampleApp({.title = "Hello World"}) {}

protected:
    bool create() override
    {
        // The box, with the default material
        _material = std::make_shared<StandardMaterial>();
        _box = createPrimitive("box", _material.get());
        _box->setName("cube");

        // The camera
        auto* camera = createCamera(Vector3(0.0f, 0.0f, 3.0f));
        camera->setName("camera");
        if (auto* cameraComponent = camera->findComponent<CameraComponent>()) {
            cameraComponent->camera()->setClearColor(Color(0.5f, 0.6f, 0.9f, 1.0f));
        }

        // The directional light
        auto* light = createDirectionalLight(Vector3(45.0f, 0.0f, 0.0f));
        light->setName("light");
        return true;
    }

    void update(const float dt) override
    {
        // Rotate the box according to the time since the last frame
        _box->rotate(10.0f * dt, 20.0f * dt, 30.0f * dt);
    }

private:
    std::shared_ptr<StandardMaterial> _material;
    Entity* _box = nullptr;
};

VISUTWIN_EXAMPLE_MAIN(HelloWorldExample)
