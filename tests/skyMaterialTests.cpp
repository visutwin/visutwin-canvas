// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2026
//
// Scene::setSkyMaterial: the sky mesh is built for a scene's sky material with no skybox
// or environment set, draws with exactly that material (render state included: the sky's
// depth-write setting must not replace the material's depth state), sits on the skybox
// layer, and is gone again once the material is cleared.

#include <algorithm>
#include <memory>

#include "framework/components/componentSystem.h"
#include "framework/engine.h"
#include "platform/graphics/depthState.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/materials/material.h"
#include "scene/meshInstance.h"
#include "scene/scene.h"
#include "scene/skybox/sky.h"
#include "scene/skybox/skyMesh.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    bool onSkyboxLayer(Scene& scene, const MeshInstance* meshInstance)
    {
        const auto layers = scene.layers();
        const auto layer = layers ? layers->getLayerById(LAYERID_SKYBOX) : nullptr;
        if (!layer) {
            return false;
        }
        const auto& instances = layer->meshInstances();
        return std::find(instances.begin(), instances.end(), meshInstance) != instances.end();
    }
}

int main()
{
    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{.cpuBuffers = true});
    auto engine = makeTestEngine<>(device);
    Scene& scene = *engine->scene();
    Sky* sky = scene.sky();
    check(sky != nullptr, "the scene has a sky");
    if (!sky) {
        return finish("sky material");
    }

    check(sky->skyMesh() == nullptr, "no skybox, environment or sky material: no sky mesh");

    auto material = std::make_shared<Material>();
    material->setIsSkybox(true);
    auto depthState = std::make_shared<DepthState>();
    depthState->setDepthWrite(false);
    material->setDepthState(depthState);
    scene.setSkyMaterial(material);

    SkyMesh* skyMesh = sky->skyMesh();
    check(skyMesh != nullptr, "a sky material alone builds the sky mesh");
    MeshInstance* meshInstance = skyMesh ? skyMesh->meshInstance() : nullptr;
    check(meshInstance && meshInstance->material() == material.get(), "the sky mesh draws with the sky material");
    check(meshInstance && onSkyboxLayer(scene, meshInstance), "the sky mesh is on the skybox layer");

    sky->setDepthWrite(true);
    check(material->depthState() == depthState, "the sky's depth-write setting leaves the material's depth state");

    scene.setSkyMaterial(nullptr);
    check(sky->skyMesh() == nullptr, "clearing the sky material removes the sky mesh");

    return finish("sky material");
}
