// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.11.2026
//
// A camera's scene depth handle carries the matrices of the frame its depth was rendered in.
// A camera frame publishes the handle in its first pass, before its forward pass has stored the
// frame's matrices on the camera (Camera::storeShaderMatrices), so a handle filled from the
// camera's stored matrices carried the PREVIOUS frame's, and a compute kernel unprojecting the
// scene depth through it (sceneDepthWorldPosition) placed every point where the camera used to
// look. Rendered by the real engine on a stub device: a camera frame with the scene depth
// requested, the camera moved every frame, and after each frame the handle's matrices held
// against the ones the forward pass gave the shaders in that frame.

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/sceneMapHandles.h"
#include "scene/camera.h"
#include "scene/materials/standardMaterial.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    // The largest difference between two matrices, element by element.
    float largestDifference(const Matrix4& a, const Matrix4& b)
    {
        float fa[16];
        float fb[16];
        a.store(fa);
        b.store(fb);
        float worst = 0.0f;
        for (int i = 0; i < 16; ++i) {
            worst = std::max(worst, std::abs(fa[i] - fb[i]));
        }
        return worst;
    }
}

int main()
{
    std::cout << std::unitbuf;
    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{
        .size = {64, 64}, .cpuBuffers = true, .recordDraws = true, .renderTargets = true});
    auto engine = makeTestEngine<RenderComponentSystem, CameraComponentSystem>(device);

    Entity* cameraEntity = addTo(engine->root(), newEntity(engine.get(), "Camera"));
    cameraEntity->setLocalPosition(0.0f, 0.0f, 10.0f);
    auto* cameraComponent = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    cameraComponent->rendering().useCameraFrame = true;
    cameraComponent->requestSceneDepthMap(true);
    Camera* camera = cameraComponent->camera();

    auto material = std::make_shared<StandardMaterial>();
    Entity* box = addTo(engine->root(), newEntity(engine.get(), "Box"));
    auto* render = static_cast<RenderComponent*>(box->addComponent<RenderComponent>());
    render->setMaterial(material.get());
    render->setType("box");

    engine->start();
    std::cout << "a camera frame with its scene depth requested, the camera moving every frame\n";
    for (int frame = 1; frame <= 3; ++frame) {
        cameraEntity->setLocalPosition(static_cast<float>(frame) * 2.0f, 0.0f, 10.0f);
        engine->update(1.0f / 60.0f);
        engine->render();

        const auto& handle = *camera->sceneDepthMapHandle();
        const std::string at = "frame " + std::to_string(frame) + ": ";
        check(handle.renderVersion == device->renderVersion() && !handle.texture.expired(),
            at + "the camera frame published this frame's depth");
        // What the forward pass gave the shaders in this frame, and the frame before.
        const float current = largestDifference(handle.viewProjectionInverse, camera->viewProjectionInverse());
        const float previous = largestDifference(handle.viewProjectionInverse,
            camera->viewProjectionPrevious().inverse());
        check(current < 1e-4f && previous > 1e-2f,
            at + "its inverse view-projection is this frame's (off by " + std::to_string(current) +
                "), not the last frame's (off by " + std::to_string(previous) + ")");
        check(largestDifference(handle.viewProjection.inverse(), camera->viewProjectionInverse()) < 1e-4f,
            at + "and so is its view-projection");
    }

    std::cout << "\nwith TAA: the published matrix carries the frame's jitter, as the forward view's does\n";
    cameraComponent->setTaaEnabled(true);
    float largestJitter = 0.0f;
    for (int frame = 4; frame <= 7; ++frame) {
        cameraEntity->setLocalPosition(static_cast<float>(frame) * 2.0f, 0.0f, 10.0f);
        engine->update(1.0f / 60.0f);
        engine->render();
        const auto& handle = *camera->sceneDepthMapHandle();
        const float current = largestDifference(handle.viewProjectionInverse, camera->viewProjectionInverse());
        largestJitter = std::max(largestJitter, std::abs(camera->jitter()));
        check(handle.renderVersion == device->renderVersion() && current < 1e-4f,
            "frame " + std::to_string(frame) + ": this frame's jittered matrix (off by " + std::to_string(current) + ")");
    }
    check(largestJitter > 0.0f, "the camera jitters under TAA, so the jitter was part of what was compared");

    return finish("scene depth publish");
}
