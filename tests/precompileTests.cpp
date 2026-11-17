// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 17.11.2026
//
// Engine::precompile on a stub device that counts what a backend would see: the frame that
// starts a precompile builds and culls the frame and runs its passes' draws into precompile
// passes (GraphicsDevice::beginPrecompilePass), with no device frame begun; the frames after it
// render nothing while the device reports pipelines still building, and the one after the last
// renders as usual, after "precompile:end" and the callback. What is off screen takes part
// unless PrecompileOptions::frustumCulling, and a one-shot shadow update is left for the frame
// that renders it. The compile queue the backends build pipelines on is checked on its own.

#include <atomic>
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
#include "platform/graphics/pipelineCompileQueue.h"
#include "scene/light.h"
#include "scene/materials/standardMaterial.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    Entity* addEntity(Engine& engine, const std::string& name)
    {
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        entity->setEngine(&engine);
        entity->setName(name);
        engine.root()->addChild(std::move(owned));
        return entity;
    }

    /// Counts draws inside and outside precompile passes; `pending` stands for the pipelines
    /// a backend would still be building.
    class PrecompileCountingDevice final : public StubGraphicsDevice
    {
    public:
        using StubGraphicsDevice::StubGraphicsDevice;

        void draw(const Primitive& primitive, const std::shared_ptr<IndexBuffer>& indexBuffer, const int numInstances,
            const int indirectSlot, const bool first, const bool last) override
        {
            if (precompilePass()) {
                ++precompileDraws;
                return;
            }
            ++renderedDraws;
            StubGraphicsDevice::draw(primitive, indexBuffer, numInstances, indirectSlot, first, last);
        }

        int precompilePendingCount() const override { return pending; }

        int precompileDraws = 0;
        int renderedDraws = 0;
        int precompilePasses = 0;
        int pending = 0;

    protected:
        void onBeginPrecompilePass(RenderPass* /*renderPass*/) override { ++precompilePasses; }
    };

    void resetCounts(PrecompileCountingDevice& device)
    {
        device.precompileDraws = device.renderedDraws = device.precompilePasses = 0;
    }
}

int main()
{
    std::cout << std::unitbuf;

    // The compile queue: every job runs once, pending() counts the unfinished, waitIdle()
    // returns once none is left, and the destructor finishes what was queued.
    {
        std::atomic<int> ran{0};
        {
            PipelineCompileQueue queue(4);
            check(queue.pending() == 0, "queue: idle before the first job");
            for (int i = 0; i < 64; ++i) {
                queue.submit([&ran] { ran.fetch_add(1); });
            }
            queue.waitIdle();
            check(ran.load() == 64 && queue.pending() == 0, "queue: 64 jobs ran, none pending (" +
                std::to_string(ran.load()) + ")");
            for (int i = 0; i < 16; ++i) {
                queue.submit([&ran] { ran.fetch_add(1); });
            }
        }
        check(ran.load() == 80, "queue: destroying it finishes the jobs it holds (" + std::to_string(ran.load()) + ")");
    }

    auto device = std::make_shared<PrecompileCountingDevice>(StubGraphicsDevice::Options{
        .size = {64, 64}, .cpuBuffers = true, .recordDraws = true, .renderTargets = true});
    auto engine = makeTestEngine<RenderComponentSystem, LightComponentSystem, CameraComponentSystem>(device);

    // One camera at z = 12 looking down -Z, a box in front of it and one behind it.
    Entity* cameraEntity = addEntity(*engine, "Camera");
    cameraEntity->setLocalPosition(0.0f, 0.0f, 12.0f);
    cameraEntity->addComponent<CameraComponent>();

    Entity* lightEntity = addEntity(*engine, "Light");
    lightEntity->setLocalEulerAngles(-60.0f, 0.0f, 0.0f);
    auto* light = static_cast<LightComponent*>(lightEntity->addComponent<LightComponent>());
    light->setType(LightType::LIGHTTYPE_DIRECTIONAL);
    light->setCastShadows(true);

    auto material = std::make_shared<StandardMaterial>();
    for (const float z : {0.0f, 30.0f}) {
        Entity* box = addEntity(*engine, z == 0.0f ? "InView" : "Behind");
        box->setLocalPosition(0.0f, 0.0f, z);
        auto* render = static_cast<RenderComponent*>(box->addComponent<RenderComponent>());
        render->setMaterial(material.get());
        render->setType("box");
        render->setCastShadows(false);
    }

    engine->start();
    engine->update(1.0f / 60.0f);
    resetCounts(*device);
    engine->render();
    const int renderedForward = device->renderedDraws;
    check(renderedForward >= 1, "a rendered frame draws (" + std::to_string(renderedForward) + ")");

    // The precompile, with three pipelines "building".
    std::vector<std::string> events;
    float lastProgress = -1.0f;
    engine->on("precompile:start", [&] { events.emplace_back("start"); });
    engine->on("precompile:progress", [&](const float progress) {
        events.emplace_back("progress");
        lastProgress = progress;
    });
    engine->on("precompile:end", [&] { events.emplace_back("end"); });
    int callbacks = 0;
    PrecompileStats reported;
    engine->precompile({}, [&](const PrecompileStats& stats) {
        ++callbacks;
        reported = stats;
    });
    check(engine->precompiling(), "precompiling() once asked");

    light->setShadowUpdateMode(ShadowUpdateType::SHADOWUPDATE_THISFRAME);
    device->pending = 3;
    const int versionBefore = device->renderVersion();
    engine->update(1.0f / 60.0f);
    resetCounts(*device);
    engine->render();
    check(device->renderVersion() == versionBefore, "the precompile frame begins no device frame");
    check(device->renderedDraws == 0, "the precompile frame renders nothing (" +
        std::to_string(device->renderedDraws) + " draws)");
    check(device->precompilePasses > 0, "its passes run as precompile passes (" +
        std::to_string(device->precompilePasses) + ")");
    const int unculledPrecompile = device->precompileDraws;
    check(device->precompileDraws > renderedForward, "it draws the box behind the camera too: " +
        std::to_string(device->precompileDraws) + " precompile draws against " + std::to_string(renderedForward) +
        " rendered");
    check(light->light()->shadowUpdateMode() == ShadowUpdateType::SHADOWUPDATE_THISFRAME,
        "the one-shot shadow update is left for the frame that renders");
    check(events.size() == 2 && events[0] == "start" && events[1] == "progress" && lastProgress == 0.0f,
        "start, then progress 0");
    check(callbacks == 0 && engine->precompiling(), "not finished while pipelines build");

    // Still building: nothing renders, nothing is precompiled again.
    device->pending = 1;
    engine->update(1.0f / 60.0f);
    resetCounts(*device);
    engine->render();
    check(device->renderedDraws == 0 && device->precompileDraws == 0 && device->renderVersion() == versionBefore,
        "a frame while pipelines build renders and precompiles nothing");
    check(events.back() == "progress" && near(lastProgress, 2.0f / 3.0f, 1e-6f), "progress 2/3 with one of three left");

    // Built: "precompile:end", the callback, and this frame renders as usual.
    device->pending = 0;
    engine->update(1.0f / 60.0f);
    resetCounts(*device);
    engine->render();
    check(events.size() >= 2 && events.back() == "end", "end once nothing builds");
    check(callbacks == 1 && !engine->precompiling(), "the callback ran once, the precompile is over");
    check(reported.time >= reported.frameTime && reported.frameTime >= 0.0, "the statistics carry the times");
    check(device->renderVersion() == versionBefore + 1 && device->renderedDraws > 0,
        "the frame after the last build renders");
    check(light->light()->shadowUpdateMode() == ShadowUpdateType::SHADOWUPDATE_NONE,
        "the rendered frame consumed the one-shot shadow update");

    // With frustum culling the precompile prepares only what the camera sees.
    engine->precompile({.frustumCulling = true});
    engine->update(1.0f / 60.0f);
    resetCounts(*device);
    engine->render();
    check(!engine->precompiling(), "a precompile with nothing building ends in its first frame");
    const int culledPrecompile = device->precompileDraws;
    check(culledPrecompile > 0 && culledPrecompile < unculledPrecompile,
        "with frustum culling the box behind the camera takes no part (" + std::to_string(culledPrecompile) + ")");
    check(device->renderedDraws == renderedForward, "and the same frame renders, as before (" +
        std::to_string(device->renderedDraws) + ")");

    // Destroyed mid-precompile: the callback still runs.
    int lateCallbacks = 0;
    device->pending = 2;
    engine->precompile({}, [&](const PrecompileStats&) { ++lateCallbacks; });
    engine->update(1.0f / 60.0f);
    engine->render();
    engine->destroy();
    check(lateCallbacks == 1, "destroying the engine mid-precompile runs the callback");

    return finish("precompile");
}
