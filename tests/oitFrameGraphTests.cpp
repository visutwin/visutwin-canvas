// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 08.11.2026
//
// Order-independent transparency in the frame graph (Layer::setOrderIndependentTransparency),
// built through the real engine on a stub device, back buffer and camera frame alike:
//   - the World layer's transparent sublayer is drawn by a RenderPassOit and by no forward pass,
//     its accumulation into a two-attachment target of the camera's size and its composite over
//     the camera's target, between the layers before and after it;
//   - the camera holds a scene depth map request while it renders the layer, and gives it back
//     when the layer turns the option off, which puts the sublayer back in a forward pass;
//   - under a camera frame, the frame's own passes carry the RenderPassOit.

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/renderTarget.h"
#include "scene/camera.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/frameGraph.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"
#include "scene/renderer/forwardRenderer.h"
#include "scene/renderer/renderPassForward.h"
#include "scene/renderer/renderPassOit.h"
#include "scene/scene.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    struct GraphSummary
    {
        int oitPasses = 0;
        int accumulations = 0;          // forward passes drawing the World's transparent sublayer
        int forwardPassesWithIt = 0;    // ... into anything but a two-attachment target
        bool accumulationSized = false;
        bool compositeOnCameraTarget = false;
    };

    GraphSummary summarize(Engine& engine, const int width, const int height)
    {
        FrameGraph frameGraph;
        engine.renderer()->buildFrameGraph(&frameGraph, engine.scene()->layers().get());
        std::vector<RenderPass*> passes;
        for (const auto& pass : frameGraph.renderPasses()) {
            passes.push_back(pass.get());
        }
        GraphSummary summary;
        for (size_t i = 0; i < passes.size(); ++i) {
            if (auto* oit = dynamic_cast<RenderPassOit*>(passes[i])) {
                ++summary.oitPasses;
                summary.accumulationSized = summary.accumulationSized ||
                    (oit->accumulationTexture() && static_cast<int>(oit->accumulationTexture()->width()) == width &&
                     static_cast<int>(oit->accumulationTexture()->height()) == height);
                if (const auto* composite = oit->compositePass()) {
                    summary.compositeOnCameraTarget = summary.compositeOnCameraTarget || composite->hasRenderTarget();
                }
            }
            auto* forward = dynamic_cast<RenderPassForward*>(passes[i]);
            if (!forward) {
                continue;
            }
            for (const auto* action : forward->renderActions()) {
                if (!action || !action->transparent || !action->layer || action->layer->id() != LAYERID_WORLD) {
                    continue;
                }
                const auto& target = forward->renderTarget();
                if (target && target->colorBufferCount() == 2) {
                    ++summary.accumulations;
                } else {
                    ++summary.forwardPassesWithIt;
                }
            }
        }
        return summary;
    }
}

int main()
{
    std::cout << std::unitbuf;

    constexpr int width = 64;
    constexpr int height = 48;
    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{
        .size = {width, height}, .cpuBuffers = true, .recordDraws = true, .renderTargets = true});
    auto engine = makeTestEngine<RenderComponentSystem, CameraComponentSystem>(device);

    Entity* cameraEntity = addTo(engine->root(), newEntity(engine.get(), "Camera"));
    cameraEntity->setLocalPosition(0.0f, 0.0f, 8.0f);
    auto* camera = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());

    auto material = std::make_shared<StandardMaterial>();
    material->setOpacity(0.5f);
    material->setTransparent(true);
    Entity* box = addTo(engine->root(), newEntity(engine.get(), "Box"));
    auto* render = static_cast<RenderComponent*>(box->addComponent<RenderComponent>());
    render->setMaterial(material.get());
    render->setType("box");

    const auto world = engine->scene()->layers()->getLayerById(LAYERID_WORLD);
    if (!check(world != nullptr, "the composition has a World layer")) {
        return finish("oit frame graph");
    }

    engine->start();
    engine->update(1.0f / 60.0f);
    engine->render();
    const bool depthBefore = camera->renderSceneDepthMap();

    std::cout << "the back buffer, order-independent transparency on the World layer\n";
    world->setOrderIndependentTransparency(true);
    engine->update(1.0f / 60.0f);
    {
        const auto summary = summarize(*engine, width, height);
        check(camera->renderSceneDepthMap(), "the camera requests its scene depth map");
        check(summary.oitPasses == 1, "one RenderPassOit");
        check(summary.accumulations == 1, "the World's transparent sublayer is accumulated into two attachments");
        check(summary.forwardPassesWithIt == 0, "and drawn by no other forward pass");
        check(summary.accumulationSized, "the accumulation is the camera's target size");
        check(summary.compositeOnCameraTarget, "the composite renders into the camera's target");
    }
    engine->render();

    std::cout << "\nturned off again\n";
    world->setOrderIndependentTransparency(false);
    engine->update(1.0f / 60.0f);
    {
        const auto summary = summarize(*engine, width, height);
        check(camera->renderSceneDepthMap() == depthBefore, "the camera gives its depth map request back");
        check(summary.oitPasses == 0 && summary.accumulations == 0, "no RenderPassOit");
        check(summary.forwardPassesWithIt == 1, "the sublayer is in a forward pass again");
    }
    engine->render();

    std::cout << "\na camera frame\n";
    auto rendering = camera->rendering();
    rendering.useCameraFrame = true;
    camera->setRendering(rendering);
    world->setOrderIndependentTransparency(true);
    engine->update(1.0f / 60.0f);
    engine->render();
    engine->update(1.0f / 60.0f);
    {
        const auto summary = summarize(*engine, width, height);
        check(summary.oitPasses == 1, "the camera frame carries one RenderPassOit");
        check(summary.accumulations == 1 && summary.forwardPassesWithIt == 0,
            "which alone draws the World's transparent sublayer");
    }

    return finish("oit frame graph");
}
