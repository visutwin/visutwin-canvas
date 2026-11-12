// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.11.2026
//
// A layer with no clustered light draws without the cluster loop. Compiled in, the loop finds no
// light in any cell and changes no pixel, yet costs every fragment the registers it keeps, so
// the forward variants of a layer take VT_FEATURE_LIGHT_CLUSTERING only while the layer's
// clustered light list is not empty. A render cannot see the difference, which is why this
// records the bound shader's features per draw: off with no local light, on with an omni light
// lighting the layer, off again once that light is disabled or lights another layer only.
//
// Rendered by the real engine on a stub device, under the default clustered lighting.

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/shader.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    // Records whether each draw's shader carries the cluster loop.
    class RecordingDevice final : public StubGraphicsDevice
    {
    public:
        using StubGraphicsDevice::StubGraphicsDevice;

        std::vector<bool> clustering;

        void draw(const Primitive& primitive, const std::shared_ptr<IndexBuffer>& indexBuffer, const int numInstances,
            int indirectSlot, bool first, bool last) override
        {
            StubGraphicsDevice::draw(primitive, indexBuffer, numInstances, indirectSlot, first, last);
            // The forward program's draws (its fragment entries are forwardFragment...).
            if (_shader && _shader->definition().fshader.starts_with("forwardFragment")) {
                clustering.push_back(_shader->features().test(ShaderFeature::LightClustering));
            }
        }
    };

    Entity* addEntity(Engine& engine, const std::string& name)
    {
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        entity->setEngine(&engine);
        entity->setName(name);
        engine.root()->addChild(std::move(owned));
        return entity;
    }
}

int main()
{
    std::cout << std::unitbuf;

    auto device = std::make_shared<RecordingDevice>(StubGraphicsDevice::Options{
        .size = {64, 64}, .cpuBuffers = true, .recordDraws = true, .renderTargets = true});
    auto engine = makeTestEngine<RenderComponentSystem, LightComponentSystem, CameraComponentSystem>(device);
    check(engine->scene()->clusteredLightingEnabled(), "clustered lighting is the default");

    Entity* cameraEntity = addEntity(*engine, "Camera");
    cameraEntity->addComponent<CameraComponent>();

    // A lit box in front of the camera (it looks down -Z from the origin).
    auto material = std::make_shared<StandardMaterial>();
    Entity* box = addEntity(*engine, "Box");
    box->setLocalPosition(0.0f, 0.0f, -5.0f);
    auto* render = static_cast<RenderComponent*>(box->addComponent<RenderComponent>());
    render->setMaterial(material.get());
    render->setType("box");

    engine->start();
    // The forward draws of one frame: whether each took the cluster loop.
    const auto frame = [&] {
        device->clustering.clear();
        engine->update(1.0f / 60.0f);
        engine->render();
        return device->clustering;
    };
    const auto all = [](const std::vector<bool>& draws, const bool value) {
        if (draws.empty()) {
            return false;
        }
        for (const bool draw : draws) {
            if (draw != value) {
                return false;
            }
        }
        return true;
    };

    frame();
    check(all(frame(), false), "no local light: the box draws without the cluster loop");

    // An omni light beside the box, lighting the World layer.
    Entity* lightEntity = addEntity(*engine, "Omni");
    lightEntity->setLocalPosition(1.0f, 1.0f, -4.0f);
    auto* omni = static_cast<LightComponent*>(lightEntity->addComponent<LightComponent>());
    omni->setType(LightType::LIGHTTYPE_OMNI);
    omni->setRange(10.0f);
    check(all(frame(), true), "an omni light lighting the layer: the box draws with the cluster loop");

    lightEntity->setEnabled(false);
    check(all(frame(), false), "the light disabled: the box draws without it again");

    lightEntity->setEnabled(true);
    check(all(frame(), true), "enabled again: with it");
    omni->setLayers({LAYERID_UI});
    check(all(frame(), false), "a light that lights another layer only: without it");

    engine->destroy();
    return finish("cluster variant");
}
