// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2026
//
// An engine destroyed with Engine::destroy() lets go of its graphics device: once the engine
// and the application's own reference are gone, nothing keeps the device alive. Its program
// library and default material sit in static per-device caches, and the library holds the
// device, so without their release the device lived until process exit, where its shaders were
// freed after its driver (an abort at exit on Vulkan from the C API's shared library). Built,
// rendered and destroyed three times over, as a frontend in a REPL would.

#include <iostream>
#include <memory>

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/null/nullGraphicsDevice.h"
#include "scene/materials/material.h"
#include "scene/shader-lib/programLibrary.h"
#include "support/check.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

int main()
{
    std::cout << std::unitbuf;
    for (int round = 1; round <= 3; ++round) {
        const std::string suffix = " (round " + std::to_string(round) + ")";
        auto device = std::make_shared<NullGraphicsDevice>(64, 64);
        const std::weak_ptr<GraphicsDevice> watch = device;
        auto engine = makeTestEngine<RenderComponentSystem, CameraComponentSystem, LightComponentSystem>(device);
        check(getProgramLibrary(device) != nullptr && getDefaultMaterial(device) != nullptr,
            "the engine installs a program library and a default material" + suffix);

        Entity* box = addTo(engine->root(), newEntity(engine.get(), "Box"));
        static_cast<RenderComponent*>(box->addComponent<RenderComponent>())->setType("box");
        Entity* camera = addTo(engine->root(), newEntity(engine.get(), "Camera"));
        camera->setLocalPosition(0.0f, 0.0f, 5.0f);
        camera->addComponent<CameraComponent>();
        engine->start();
        engine->update(1.0f / 60.0f);
        engine->render();

        engine->destroy();
        check(getProgramLibrary(device) == nullptr && getDefaultMaterial(device) == nullptr,
            "destroying the engine releases both" + suffix);
        engine.reset();
        device.reset();
        check(watch.expired(), "and nothing keeps the device alive after it" + suffix);
    }
    return finish("engine-lifetime");
}
