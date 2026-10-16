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
#include "platform/graphics/texture.h"
#include "scene/camera.h"
#include "scene/graphics/cameraFrameEffect.h"
#include "scene/graphics/renderPassCameraFrame.h"
#include "scene/graphics/renderPassColorGrab.h"
#include "scene/graphics/renderPassCompose.h"
#include "scene/graphics/renderPassDownsample.h"
#include "scene/graphics/renderPassPrepass.h"
#include "scene/graphics/renderPassTAA.h"
#include "scene/graphics/smaaEffect.h"
#include "scene/graphics/taaEffect.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"
#include "scene/renderer/forwardRenderer.h"
#include "scene/scene.h"
#include "scene/skybox/sky.h"
#include "scene/skybox/skyMesh.h"
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
                    : stage == "postScene" ? passes.postScene : stage == "resolve" ? passes.resolve
                    : passes.postResolve;
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
        bool rendersSky() const override { return sky; }
        float cameraJitter() const override { return jitter; }
        void resolveScene(CameraFrameEffectScene& scene) override
        {
            resolvedFrom = scene.texture;
            if (resolveTo) {
                scene.texture = resolveTo;
            }
        }

        std::vector<std::string> stages;
        std::string key;
        bool sky = false;
        float jitter = 0.0f;
        Texture* resolveTo = nullptr;
        Texture* resolvedFrom = nullptr;
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
        std::vector<std::string>{"postResolve", "resolve", "postScene", "postOpaque", "preScene"});
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
        const int temporal = indexOfLabel(passes, "stages:postResolve");
        const int resolve = indexOfLabel(passes, "stages:resolve");
        const int firstForward = indexOfType<RenderPassForward>(passes);
        const int grab = indexOfType<RenderPassColorGrab>(passes);
        const int transparent = firstForward >= 0 ? indexOfType<RenderPassForward>(passes, firstForward + 1) : -1;
        const int compose = indexOfType<RenderPassCompose>(passes);
        check(pre >= 0 && opaque >= 0 && scene >= 0 && temporal >= 0, "every stage's pass is in the frame");
        check(pre >= 0 && pre < firstForward, "preScene runs before the scene pass");
        check(grab > firstForward && opaque == grab + 1, "postOpaque runs right after the colour copy");
        check(transparent == opaque + 1, "and right before the transparent scene pass, which it split off");
        check(scene > transparent && resolve == scene + 1, "resolve runs right after postScene");
        check(resolve < temporal, "postResolve runs after the resolve stage");
        check(temporal >= 0 && temporal == compose - 1, "postResolve runs right before the composition");
        check(indexOfExecuted("stages:preScene") >= 0 &&
              indexOfExecuted("stages:preScene") < indexOfExecuted("stages:postOpaque") &&
              indexOfExecuted("stages:postOpaque") < indexOfExecuted("stages:postScene") &&
              indexOfExecuted("stages:postScene") < indexOfExecuted("stages:resolve") &&
              indexOfExecuted("stages:resolve") < indexOfExecuted("stages:postResolve"),
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
        check(stages->lastContext.renderWidth == 64 && stages->lastContext.renderHeight == 48,
            "and the size the scene renders at");
        check(r.sceneColor != nullptr && stages->resolvedFrom == r.sceneColor,
            "the first resolving effect reads the scene render target's colour");
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
            std::vector<std::string>{"postResolve"});
        camera->addFrameEffect(extra);
        frame();
        cameraFrame = camera->cameraFrame();
        const auto& passes = cameraFrame->beforePasses();
        check(indexOfType<RenderPassPrepass>(passes) >= 0, "requiring the prepass depth renders the prepass");
        check(extra->lastResources.sceneColorHalf != nullptr && indexOfType<RenderPassDownsample>(passes) >= 0,
            "requiring the half-resolution scene makes the copy and hands it over");
        check(indexOfLabel(passes, "extra:postResolve") > indexOfLabel(passes, "stages:postResolve"),
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
        check(indexOfLabel(camera->cameraFrame()->beforePasses(), "extra:postResolve") < 0,
            "and are gone from the frame");
        camera->removeFrameEffect(extra);
        camera->removeFrameEffect(stages);
        frame();
        check(stages->livePasses.empty() && stages->destroyed == stages->created,
            "a removed effect's passes are let go of at the next frame");
        const auto& after = camera->cameraFrame()->beforePasses();
        check(indexOfType<RenderPassColorGrab>(after) < 0, "and the colour copy it asked for is no longer made");
    }

    std::cout << "compose effects\n";
    {
        // A pass-owning effect to count rebuilds by, and a compose-only one.
        auto owner = std::make_shared<TestEffect>("owner", std::vector<FrameResource>{},
            std::vector<std::string>{"postScene"});
        camera->addFrameEffect(owner);
        auto rendering = camera->rendering();
        rendering.bloomIntensity = 0.0f;   // nothing else to post-process
        camera->setRendering(rendering);
        frame();

        class CountingCompose final : public CameraFrameEffect
        {
        public:
            CountingCompose()
                : CameraFrameEffect("tint", CameraFrameEffectOptions{.slot = ComposeSlot::Ldr,
                      .slang = "float3 applyTint(float3 c, float2 uv) { return c * tintScale; }"}) {}
            void update() override
            {
                ++updates;
                setUniform("tintScale", 0.5f);
            }
            int updates = 0;
        };
        auto tint = std::make_shared<CountingCompose>();
        camera->removeFrameEffect(owner);
        frame();
        camera->addFrameEffect(owner);
        frame();
        const int rebuiltBefore = owner->created;
        camera->addFrameEffect(tint);
        frame();
        frame();
        check(tint->updates == 2, "a compose effect's update() runs every frame");
        check(owner->created == rebuiltBefore, "adding a compose-only effect rebuilds no pass");
        tint->setEnabled(false);
        frame();
        tint->setEnabled(true);
        frame();
        check(owner->created == rebuiltBefore, "nor does turning it off and on");

        camera->removeFrameEffect(owner);
        frame();
        check(camera->onPostprocessing() != nullptr && camera->cameraFrame() != nullptr,
            "a compose effect alone makes the camera render through a frame");

        auto depthTint = std::make_shared<CameraFrameEffect>("depthTint", CameraFrameEffectOptions{
            .slot = ComposeSlot::Hdr, .slang = "float3 applyDepthTint(float3 c, float2 uv) { return c; }",
            .requiredResources = {FrameResource::Depth}});
        camera->addFrameEffect(owner);
        frame();
        const int beforeDepth = owner->created;
        camera->addFrameEffect(depthTint);
        frame();
        check(owner->created == beforeDepth + 1, "a compose effect requiring a resource does rebuild the frame");
        camera->removeFrameEffect(depthTint);
        camera->removeFrameEffect(tint);
        camera->removeFrameEffect(owner);
        frame();
    }

    std::cout << "the resolve stage\n";
    {
        TextureOptions resolvedOptions;
        resolvedOptions.width = 64;
        resolvedOptions.height = 48;
        resolvedOptions.name = "resolved";
        auto resolvedTexture = std::make_shared<Texture>(device.get(), resolvedOptions);
        auto first = std::make_shared<TestEffect>("first", std::vector<FrameResource>{},
            std::vector<std::string>{"resolve"});
        auto second = std::make_shared<TestEffect>("second", std::vector<FrameResource>{},
            std::vector<std::string>{"resolve"});
        first->resolveTo = resolvedTexture.get();
        camera->addFrameEffect(first);
        camera->addFrameEffect(second);
        frame();
        auto cameraFrameNow = camera->cameraFrame();
        const auto& passes = cameraFrameNow->beforePasses();
        check(indexOfLabel(passes, "first:resolve") >= 0 &&
              indexOfLabel(passes, "first:resolve") < indexOfLabel(passes, "second:resolve"),
            "resolving passes run in registration order");
        check(second->resolvedFrom == resolvedTexture.get(), "an effect reads what the one before it resolved");
        check(second->lastContext.sceneTexture == resolvedTexture.get(), "and the frame reads the last of them");
        const auto* compose = dynamic_cast<RenderPassCompose*>(passes[indexOfType<RenderPassCompose>(passes)].get());
        check(compose && compose->sceneTexture() == resolvedTexture.get(), "the composition reads the resolved scene");

        std::cout << "camera jitter\n";
        check(camera->camera()->jitter() == 0.0f, "no effect asks for jitter: the camera does not jitter");
        second->jitter = 0.6f;
        first->jitter = 0.25f;
        frame();
        check(near(camera->camera()->jitter(), 0.6f, 0.0f), "the camera jitters as much as the most demanding effect");
        second->setEnabled(false);
        frame();
        check(near(camera->camera()->jitter(), 0.25f, 0.0f), "an inactive effect asks for nothing");
        camera->removeFrameEffect(first);
        camera->removeFrameEffect(second);
        frame();
        check(camera->camera()->jitter() == 0.0f, "nor does a removed one");

        std::cout << "the built-in TAA\n";
        auto keep = std::make_shared<TestEffect>("keep", std::vector<FrameResource>{},
            std::vector<std::string>{"resolve"});
        camera->addFrameEffect(keep);
        auto taa = camera->taa();
        taa.enabled = true;
        taa.jitter = 0.8f;
        camera->setTaa(taa);
        frame();
        frame();
        const auto& taaPass = camera->taaEffect()->pass();
        const auto& withTaa = camera->cameraFrame()->beforePasses();
        int taaIndex = -1;
        for (int i = 0; i < static_cast<int>(withTaa.size()); ++i) {
            if (withTaa[i] == taaPass) {
                taaIndex = i;
            }
        }
        check(taaPass != nullptr && taaIndex >= 0, "TAA is a pass of the frame");
        check(taaIndex >= 0 && taaIndex + 1 == indexOfLabel(withTaa, "keep:resolve"),
            "at the resolve stage, ahead of the registered effects");
        check(camera->frameEffects().size() == 1, "and not in the camera's effect list");
        check(near(camera->camera()->jitter(), 0.8f, 0.0f), "the camera jitters by the TAA setting");
        check(keep->resolvedFrom != nullptr && keep->resolvedFrom != keep->lastResources.sceneColor,
            "the next effect reads TAA's history, not the scene");
        keep->key = "rebuilt";
        frame();
        check(camera->taaEffect()->pass() == taaPass, "a rebuild keeps TAA's pass and its history");
        taa.enabled = false;
        camera->setTaa(taa);
        frame();
        check(camera->taaEffect()->pass() == nullptr, "turning TAA off releases them");
        camera->removeFrameEffect(keep);
        frame();

        std::cout << "SMAA\n";
        auto smaa = std::make_shared<SmaaEffect>();
        camera->addFrameEffect(smaa);
        frame();
        const auto& withSmaa = camera->cameraFrame()->beforePasses();
        const auto indexOfName = [&](const std::string& name) {
            for (int i = 0; i < static_cast<int>(withSmaa.size()); ++i) {
                if (withSmaa[i] && withSmaa[i]->name() == name) {
                    return i;
                }
            }
            return -1;
        };
        const int edge = indexOfName("RenderPassSmaa-edge");
        const int weights = indexOfName("RenderPassSmaa-weights");
        const int blend = indexOfName("RenderPassSmaa-blend");
        check(edge >= 0 && weights == edge + 1 && blend == weights + 1, "three passes, in order");
        check(blend >= 0 && blend < indexOfType<RenderPassCompose>(withSmaa), "at the resolve stage, before compose");
        const auto* smaaCompose = dynamic_cast<RenderPassCompose*>(
            withSmaa[indexOfType<RenderPassCompose>(withSmaa)].get());
        check(smaaCompose && smaa->outputTexture() && smaaCompose->sceneTexture() == smaa->outputTexture(),
            "the composition reads SMAA's output");
        check(smaa->areaTexture() && smaa->areaTexture()->width() == 160 && smaa->areaTexture()->height() == 80 &&
              smaa->areaTexture()->format() == PixelFormat::PIXELFORMAT_RG8, "the area lookup is 160 x 80 RG8");
        check(smaa->searchTexture() && smaa->searchTexture()->width() == 64 &&
              smaa->searchTexture()->height() == 16 && smaa->searchTexture()->format() == PixelFormat::PIXELFORMAT_R8,
            "the search lookup is 64 x 16 R8");
        check(smaa->edgesTexture() && smaa->edgesTexture()->width() == 64 && smaa->edgesTexture()->height() == 48 &&
              smaa->outputTexture()->width() == 64, "its targets have the scene's size");
        Texture* area = smaa->areaTexture();
        smaa->threshold = 0.05f;
        frame();
        check(smaa->areaTexture() == area, "a parameter change rebuilds nothing");
        auto keepFrame = camera->rendering();
        keepFrame.bloomIntensity = 0.02f;   // keeps the frame when the effect goes
        camera->setRendering(keepFrame);
        camera->removeFrameEffect(smaa);
        frame();
        check(smaa->outputTexture() == nullptr, "removed, its passes are let go of at the next frame");
        keepFrame.bloomIntensity = 0.0f;
        camera->setRendering(keepFrame);
        frame();
    }

    std::cout << "the time of the frame\n";
    {
        auto timed = std::make_shared<TestEffect>("timed", std::vector<FrameResource>{},
            std::vector<std::string>{"postScene"});
        camera->addFrameEffect(timed);
        frame();
        frame();
        check(near(timed->lastContext.dt, 1.0f / 60.0f, 1e-6f), "dt is the update step since the last render");
        engine->update(1.0f / 60.0f);
        engine->update(1.0f / 60.0f);
        engine->render();
        check(near(timed->lastContext.dt, 2.0f / 60.0f, 1e-6f), "every update since the last render counts");
        engine->render();
        check(timed->lastContext.dt == 0.0f, "a frame rendered again without an update advances by nothing");
        engine->setTimeScale(0.0f);
        frame();
        check(timed->lastContext.dt == 0.0f, "nor does a paused world");
        engine->setTimeScale(0.5f);
        frame();
        check(near(timed->lastContext.dt, 0.5f / 60.0f, 1e-6f), "the step is time-scaled, as scripts get it");
        engine->setTimeScale(1.0f);

        std::cout << "an effect rendering the sky\n";
        Scene& scene = *engine->scene();
        auto skyMaterial = std::make_shared<Material>();
        skyMaterial->setIsSkybox(true);
        scene.setSkyMaterial(skyMaterial);
        const auto skyboxLayer = scene.layers()->getLayerById(LAYERID_SKYBOX);
        const auto skyCulled = [&] {
            const auto& culled = engine->renderer()->culledInstances(camera->camera(), cameraEntity,
                skyboxLayer.get());
            const MeshInstance* sky = scene.sky()->skyMesh() ? scene.sky()->skyMesh()->meshInstance() : nullptr;
            const auto has = [&](const std::vector<MeshInstance*>& list) {
                return std::find(list.begin(), list.end(), sky) != list.end();
            };
            return sky && (has(culled.opaque) || has(culled.transparent));
        };
        frame();
        check(camera->camera()->skyVisible() && skyCulled(), "the camera draws the scene's sky");
        timed->sky = true;
        frame();
        check(!camera->camera()->skyVisible() && !skyCulled(), "not while an effect renders the sky in its place");
        timed->setEnabled(false);
        frame();
        check(camera->camera()->skyVisible() && skyCulled(), "and again once that effect is inactive");
        scene.sky()->setEnabled(false);
        check(scene.sky()->skyMesh() == nullptr, "a disabled sky builds no sky mesh");
        scene.sky()->setEnabled(true);
        check(scene.sky()->skyMesh() != nullptr, "enabled again, it builds one");
        scene.setSkyMaterial(nullptr);
        camera->removeFrameEffect(timed);
        frame();
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
