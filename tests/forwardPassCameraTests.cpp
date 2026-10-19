// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.10.2026
//
// A forward pass renders ONE camera. Two cameras drawing into the same target (a second
// view in a corner of the window, say) get a pass each, even when neither has directional
// shadows to render between them. A forward pass clears only for its first render action,
// so a second camera folded into the first one's pass lost its own clear: its viewport was
// drawn over whatever the first camera left there.
//
// Built through the real engine on a stub device: the frame graph the renderer builds for two
// cameras on the back buffer, read pass by pass.

#include <iostream>
#include <memory>
#include <vector>

#include "core/math/vector4.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "scene/camera.h"
#include "scene/composition/layerComposition.h"
#include "scene/frameGraph.h"
#include "scene/materials/standardMaterial.h"
#include "scene/renderer/forwardRenderer.h"
#include "scene/renderer/renderPassForward.h"
#include "scene/scene.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

int main()
{
    std::cout << std::unitbuf;

    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{
        .size = {64, 48}, .cpuBuffers = true, .recordDraws = true, .renderTargets = true});
    auto engine = makeTestEngine<RenderComponentSystem, CameraComponentSystem>(device);

    Entity* mainEntity = addTo(engine->root(), newEntity(engine.get(), "Main"));
    mainEntity->setLocalPosition(0.0f, 0.0f, 8.0f);
    auto* mainCamera = static_cast<CameraComponent*>(mainEntity->addComponent<CameraComponent>());

    // A second view in the top-right quarter, drawn after the main one and clearing its rect.
    Entity* insetEntity = addTo(engine->root(), newEntity(engine.get(), "Inset"));
    insetEntity->setLocalPosition(8.0f, 0.0f, 0.0f);
    auto* insetCamera = static_cast<CameraComponent*>(insetEntity->addComponent<CameraComponent>());
    insetCamera->setPriority(1);
    insetCamera->camera()->setRect(Vector4(0.5f, 0.5f, 0.5f, 0.5f));
    insetCamera->camera()->setClearColorBuffer(true);

    auto material = std::make_shared<StandardMaterial>();
    Entity* box = addTo(engine->root(), newEntity(engine.get(), "Box"));
    auto* render = static_cast<RenderComponent*>(box->addComponent<RenderComponent>());
    render->setMaterial(material.get());
    render->setType("box");

    engine->start();
    engine->update(1.0f / 60.0f);
    engine->render();

    FrameGraph frameGraph;
    engine->renderer()->buildFrameGraph(&frameGraph, engine->scene()->layers().get());

    std::vector<RenderPassForward*> passes;
    for (const auto& pass : frameGraph.renderPasses()) {
        if (auto* forward = dynamic_cast<RenderPassForward*>(pass.get())) {
            passes.push_back(forward);
        }
    }

    std::cout << "two cameras on the back buffer, no directional shadows\n";
    bool singleCamera = !passes.empty();
    int mainPasses = 0;
    int insetPasses = 0;
    for (const auto* pass : passes) {
        for (const auto* action : pass->renderActions()) {
            singleCamera = singleCamera && action && action->camera == pass->camera();
        }
        mainPasses += pass->camera() == mainCamera ? 1 : 0;
        insetPasses += pass->camera() == insetCamera ? 1 : 0;
    }
    check(singleCamera, "every forward pass holds the render actions of its own camera only");
    check(mainPasses >= 1 && insetPasses >= 1, "each camera has a forward pass of its own");

    // The inset camera's clearing action is the first of its pass, where the pass clears.
    bool insetClearsFirst = false;
    for (const auto* pass : passes) {
        if (pass->camera() == insetCamera && !pass->renderActions().empty()) {
            const auto* first = pass->renderActions().front();
            insetClearsFirst = insetClearsFirst || (first->firstCameraUse && first->clearColor);
        }
    }
    check(insetClearsFirst, "the inset camera's colour clear opens a pass, so it is performed");

    return finish("forward pass camera");
}
