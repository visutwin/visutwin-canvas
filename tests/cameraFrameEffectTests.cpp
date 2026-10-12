// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2026
//
// Frame effects (CameraComponent::addFrameEffect) through the real engine on a stub device:
// each stage's passes land where the stage says among the camera frame's passes and run in
// that order; requiring a resource turns on what produces it and hands it over; the passes
// are created once and rebuilt only when the active set, a requirement or a build key
// changes; and every pass an effect created is let go of on a rebuild, when the effect is
// disabled or removed, and when the camera goes away.
//
// A stage placed one pass off still renders, and an effect asking for the scene colour copy
// without getting one draws nothing visible, so neither shows in a frame no one compares;
// nor does an effect whose passes are created anew every frame.

#include <algorithm>
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
#include "platform/graphics/renderPass.h"
#include "scene/graphics/cameraFrameEffect.h"
#include "scene/graphics/renderPassCameraFrame.h"
#include "scene/graphics/renderPassColorGrab.h"
#include "scene/graphics/renderPassCompose.h"
#include "scene/graphics/renderPassDownsample.h"
#include "scene/graphics/renderPassPrepass.h"
#include "scene/materials/standardMaterial.h"
#include "scene/renderer/renderPassForward.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    std::vector<std::string> executed;

    class RecordingPass final : public RenderPass
    {
    public:
        RecordingPass(const std::shared_ptr<GraphicsDevice>& device, std::string label)
            : RenderPass(device), label(std::move(label)) {}
        void execute() override { executed.push_back(label); }
        std::string label;
    };

    class TestEffect final : public CameraFrameEffect
    {
    public:
        TestEffect(std::string id, std::vector<FrameResource> resources, std::vector<std::string> stages)
            : CameraFrameEffect(std::move(id), std::move(resources)), stages(std::move(stages)) {}

        bool ownsPasses() const override { return !stages.empty(); }

        void createPasses(const CameraFrameEffectResources& resources, CameraFrameEffectPasses& passes) override
        {
            ++created;
            lastResources = resources;
            for (const auto& stage : stages) {
                auto pass = std::make_shared<RecordingPass>(resources.device, id() + ":" + stage);
                livePasses.push_back(pass);
                auto& list = stage == "preScene" ? passes.preScene : stage == "postOpaque" ? passes.postOpaque
                    : stage == "postScene" ? passes.postScene : passes.postTemporal;
                list.push_back(pass);
            }
        }

        void destroyPasses() override
        {
            ++destroyed;
            livePasses.clear();
        }

        std::string buildKey() const override { return key; }
        void update() override { ++updates; }
        void frameUpdate(const CameraFrameEffectContext& context) override { lastContext = context; }

        std::vector<std::string> stages;
        std::string key;
        int created = 0;
        int destroyed = 0;
        int updates = 0;
        CameraFrameEffectResources lastResources;
        CameraFrameEffectContext lastContext;
        std::vector<std::shared_ptr<RenderPass>> livePasses;
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

    template <typename T>
    int indexOfType(const std::vector<std::shared_ptr<RenderPass>>& passes, const int from = 0)
    {
        for (int i = from; i < static_cast<int>(passes.size()); ++i) {
            if (dynamic_cast<T*>(passes[i].get())) {
                return i;
            }
        }
        return -1;
    }

    int indexOfLabel(const std::vector<std::shared_ptr<RenderPass>>& passes, const std::string& label)
    {
        for (int i = 0; i < static_cast<int>(passes.size()); ++i) {
            if (const auto* recording = dynamic_cast<RecordingPass*>(passes[i].get());
                recording && recording->label == label) {
                return i;
            }
        }
        return -1;
    }

    int indexOfExecuted(const std::string& label)
    {
        const auto it = std::find(executed.begin(), executed.end(), label);
        return it == executed.end() ? -1 : static_cast<int>(it - executed.begin());
    }
}

int main()
{
    std::cout << std::unitbuf;

    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{
        .size = {64, 48}, .cpuBuffers = true, .recordDraws = true, .renderTargets = true});
    auto engine = makeTestEngine<RenderComponentSystem, CameraComponentSystem>(device);

    Entity* cameraEntity = addEntity(*engine, "Camera");
    cameraEntity->setLocalPosition(0.0f, 0.0f, 8.0f);
    auto* camera = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());

    auto material = std::make_shared<StandardMaterial>();
    Entity* box = addEntity(*engine, "Box");
    auto* render = static_cast<RenderComponent*>(box->addComponent<RenderComponent>());
    render->setMaterial(material.get());
    render->setType("box");

    engine->start();
    const auto frame = [&] {
        executed.clear();
        engine->update(1.0f / 60.0f);
        engine->render();
    };

    std::cout << "registration\n";
    auto stages = std::make_shared<TestEffect>("stages",
        std::vector<FrameResource>{FrameResource::SceneColorGrab, FrameResource::SceneTarget, FrameResource::Depth},
        std::vector<std::string>{"postTemporal", "postScene", "postOpaque", "preScene"});
    check(camera->onPostprocessing() == nullptr, "a camera with nothing to post-process renders without a frame");
    check(camera->addFrameEffect(stages), "an effect is registered");
    check(!camera->addFrameEffect(std::make_shared<TestEffect>("stages", std::vector<FrameResource>{},
        std::vector<std::string>{})), "a second effect with the same id is refused");
    check(camera->frameEffect("stages") == stages && camera->frameEffects().size() == 1, "found by its id");
    check(camera->onPostprocessing() != nullptr, "an active effect makes the camera render through a frame");

    frame();
    auto cameraFrame = camera->cameraFrame();
    check(cameraFrame != nullptr, "the camera frame exists");
    if (!cameraFrame) {
        return finish("camera frame effects");
    }

    std::cout << "stages\n";
    {
        const auto& passes = cameraFrame->beforePasses();
        const int pre = indexOfLabel(passes, "stages:preScene");
        const int opaque = indexOfLabel(passes, "stages:postOpaque");
        const int scene = indexOfLabel(passes, "stages:postScene");
        const int temporal = indexOfLabel(passes, "stages:postTemporal");
        const int firstForward = indexOfType<RenderPassForward>(passes);
        const int grab = indexOfType<RenderPassColorGrab>(passes);
        const int transparent = firstForward >= 0 ? indexOfType<RenderPassForward>(passes, firstForward + 1) : -1;
        const int compose = indexOfType<RenderPassCompose>(passes);
        check(pre >= 0 && opaque >= 0 && scene >= 0 && temporal >= 0, "every stage's pass is in the frame");
        check(pre >= 0 && pre < firstForward, "preScene runs before the scene pass");
        check(grab > firstForward && opaque == grab + 1, "postOpaque runs right after the colour copy");
        check(transparent == opaque + 1, "and right before the transparent scene pass, which it split off");
        check(scene > transparent && scene < temporal, "postScene runs after the whole scene, before postTemporal");
        check(temporal >= 0 && temporal == compose - 1, "postTemporal runs right before the composition");
        check(indexOfExecuted("stages:preScene") >= 0 &&
              indexOfExecuted("stages:preScene") < indexOfExecuted("stages:postOpaque") &&
              indexOfExecuted("stages:postOpaque") < indexOfExecuted("stages:postScene") &&
              indexOfExecuted("stages:postScene") < indexOfExecuted("stages:postTemporal"),
            "the frame graph runs them in stage order");
    }

    std::cout << "resources\n";
    {
        const auto& r = stages->lastResources;
        check(r.device == device && r.camera == camera, "the device and the camera are handed over");
        check(r.depth != nullptr, "the scene depth is handed over");
        check(r.sceneTarget && !r.sceneTarget->hasDepth() && r.sceneTarget->colorBuffer() != nullptr,
            "single-sampled, the scene target is the colour alone, so the depth can be sampled");
        check(static_cast<bool>(r.sceneColorGrab) && r.sceneColorGrab() != nullptr,
            "requiring the colour copy makes one, and the handle finds it once it ran");
        check(r.sceneColorGrab() == device->sceneColorMap(), "the handle answers the copy the frame published");
        check(stages->lastContext.sceneWidth == 64 && stages->lastContext.sceneHeight == 48 &&
              stages->lastContext.sceneTexture != nullptr, "frameUpdate gets this frame's scene and its size");
        check(indexOfType<RenderPassPrepass>(cameraFrame->beforePasses()) < 0,
            "the scene depth alone renders no prepass when single-sampled");
    }

    std::cout << "rebuilds\n";
    {
        frame();
        frame();
        check(stages->created == 1 && stages->destroyed == 0, "the passes are created once across frames");
        check(stages->updates >= 3, "update() runs every frame");
        stages->key = "wider";
        frame();
        check(stages->created == 2 && stages->destroyed == 1, "a new build key rebuilds the passes");
        frame();
        check(stages->created == 2, "and then they stay");
    }

    std::cout << "required resources switch producers on\n";
    {
        auto rendering = camera->rendering();
        rendering.bloomIntensity = 0.02f;   // keeps the frame when the effect goes
        camera->setRendering(rendering);
        auto extra = std::make_shared<TestEffect>("extra",
            std::vector<FrameResource>{FrameResource::PrepassDepth, FrameResource::SceneColorHalf},
            std::vector<std::string>{"postTemporal"});
        camera->addFrameEffect(extra);
        frame();
        cameraFrame = camera->cameraFrame();
        const auto& passes = cameraFrame->beforePasses();
        check(indexOfType<RenderPassPrepass>(passes) >= 0, "requiring the prepass depth renders the prepass");
        check(extra->lastResources.sceneColorHalf != nullptr && indexOfType<RenderPassDownsample>(passes) >= 0,
            "requiring the half-resolution scene makes the copy and hands it over");
        check(indexOfLabel(passes, "extra:postTemporal") > indexOfLabel(passes, "stages:postTemporal"),
            "passes of one stage keep their effects' registration order");
        check(stages->created == 3 && stages->destroyed == 2, "adding an effect rebuilds the others' passes");

        CameraFrameOptions options;
        options.prepassEnabled = true;
        options.effectDepth = true;
        check(!RenderPassCameraFrame::prepassRenders(options), "the scene depth alone needs no prepass");
        options.effectPrepassDepth = true;
        check(RenderPassCameraFrame::prepassRenders(options), "the prepass depth does");

        extra->setEnabled(false);
        frame();
        check(extra->destroyed == 1 && extra->livePasses.empty(), "a disabled effect's passes are let go of");
        check(indexOfLabel(camera->cameraFrame()->beforePasses(), "extra:postTemporal") < 0,
            "and are gone from the frame");
        camera->removeFrameEffect(extra);
        camera->removeFrameEffect(stages);
        frame();
        check(stages->livePasses.empty() && stages->destroyed == stages->created,
            "a removed effect's passes are let go of at the next frame");
        const auto& after = camera->cameraFrame()->beforePasses();
        check(indexOfType<RenderPassColorGrab>(after) < 0, "and the colour copy it asked for is no longer made");
    }

    std::cout << "the camera going away\n";
    {
        auto last = std::make_shared<TestEffect>("last", std::vector<FrameResource>{},
            std::vector<std::string>{"postScene"});
        camera->addFrameEffect(last);
        frame();
        check(last->created == 1 && !last->livePasses.empty(), "the effect has passes");
        cameraEntity->destroy();
        check(last->destroyed == 1 && last->livePasses.empty(), "destroying the camera lets go of them");
    }

    return finish("camera frame effects");
}
