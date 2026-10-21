// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 25.09.2026
//
// FrameGraph::compile on a stub device: the load/store contract it derives between
// passes that share a target, pass merging, before-pass ordering, and that its edits
// to a PERSISTENT pass's attachment flags last one frame. Also the onBefore / onAfter
// callbacks: where a pass runs them, and that the graph merges no pass across one. And the
// ComputePass (its dispatch, its place between device passes, that nothing merges across it)
// and a camera's after-passes, scheduled by the real renderer.
//
// Passes persist across frames while the graph is rebuilt every frame. A compile() that
// raises a store or drops a cubemap mip generation and never puts it back makes one
// frame's adjacency stick to a pass for good. compile() records each edit and undoes it
// at the next compile, leaving alone any flag the pass changed itself.

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <algorithm>

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/computePass.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/camera.h"
#include "scene/composition/layerComposition.h"
#include "scene/frameGraph.h"
#include "scene/graphics/renderPassColorGrab.h"
#include "scene/graphics/renderPassDepthGrab.h"
#include "scene/materials/standardMaterial.h"
#include "scene/renderer/forwardRenderer.h"
#include "scene/renderer/renderPassForward.h"
#include "scene/scene.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    std::shared_ptr<GraphicsDevice> device;

    // A pass on `target` (null = back buffer) that DISCARDS its colour and depth unless
    // told otherwise: what an MSAA or intermediate pass looks like, so a raised store is
    // visible. `clear` makes it clear both, i.e. not read what an earlier pass left.
    std::shared_ptr<RenderPass> pass(const std::shared_ptr<RenderTarget>& target, const bool clear)
    {
        auto p = std::make_shared<RenderPass>(device);
        p->init(target);
        for (const auto& ops : p->colorArrayOps()) {
            ops->store = false;
            ops->clear = clear;
        }
        p->depthStencilOps()->storeDepth = false;
        p->depthStencilOps()->clearDepth = clear;
        p->depthStencilOps()->clearStencil = clear;
        return p;
    }

    void compile(const std::vector<std::shared_ptr<RenderPass>>& passes)
    {
        FrameGraph graph;   // a fresh graph per frame, as the engine builds one
        for (const auto& p : passes) {
            graph.addRenderPass(p);
        }
        graph.compile();
    }

    // A pass that records each frameUpdate in `log` under its name: the order the graph
    // schedules passes in, and which ones it skipped.
    class LoggingPass : public RenderPass
    {
    public:
        LoggingPass(const std::shared_ptr<GraphicsDevice>& graphicsDevice, const std::string& name, std::vector<std::string>& log)
            : RenderPass(graphicsDevice), _name(name), _log(log) {}
        void frameUpdate() const override { _log.push_back(_name); }
    private:
        std::string _name;
        std::vector<std::string>& _log;
    };

    // A pass that logs its before / execute / after and its callbacks, each callback tagged with
    // whether a device pass was open when it ran.
    class RecordingPass : public RenderPass
    {
    public:
        RecordingPass(const std::shared_ptr<GraphicsDevice>& graphicsDevice, std::vector<std::string>& log)
            : RenderPass(graphicsDevice), _log(log) {}
        void before() override { _log.push_back("before"); }
        void execute() override { _log.push_back(device()->insideRenderPass() ? "execute" : "execute(outside)"); }
        void after() override { _log.push_back("after"); }
    private:
        std::vector<std::string>& _log;
    };

    // A stub that logs device passes and compute dispatches, in the order they arrive.
    class DispatchLoggingDevice final : public StubGraphicsDevice
    {
    public:
        explicit DispatchLoggingDevice(std::vector<std::string>& log)
            : StubGraphicsDevice(Options{.size = {64, 64}, .recordDraws = true}), _log(log) {}
        bool supportsCompute() const override { return true; }
        void startRenderPass(RenderPass* pass) override
        {
            StubGraphicsDevice::startRenderPass(pass);
            _log.push_back("start");
        }
        void endRenderPass(RenderPass* pass) override
        {
            StubGraphicsDevice::endRenderPass(pass);
            _log.push_back("end");
        }
        void computeDispatch(const std::vector<Compute*>& computes, const std::string& label) override
        {
            std::string entry = insideRenderPass() ? "dispatch(inside)" : "dispatch";
            entry += "[" + label;
            for (const auto* compute : computes) {
                entry += " " + compute->name();
            }
            _log.push_back(entry + "]");
        }
    private:
        std::vector<std::string>& _log;
    };

    std::string joined(const std::vector<std::string>& log)
    {
        std::string out;
        for (const auto& name : log) {
            out += (out.empty() ? "" : " ") + name;
        }
        return out;
    }

    std::shared_ptr<RenderTarget> makeTarget(Texture* colour, const int face = 0)
    {
        RenderTargetOptions options;
        options.graphicsDevice = device.get();
        options.colorBuffer = colour;
        options.depth = true;
        options.face = face;
        return std::make_shared<StubRenderTarget>(options);
    }
}

int main()
{
    std::cout << std::unitbuf;
    device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{.size = {64, 64}});

    TextureOptions colourOptions;
    colourOptions.width = 64;
    colourOptions.height = 64;
    colourOptions.mipmaps = false;
    Texture colour(device.get(), colourOptions);
    const auto target = makeTarget(&colour);
    // A pass on another target between two passes keeps them from MERGING, which would
    // end the first with the second's stores: the store propagation cases use one.
    Texture otherColour(device.get(), colourOptions);
    const auto otherTarget = makeTarget(&otherColour);
    const auto between = [&] { return pass(otherTarget, true); };

    std::cout << "store propagation\n";
    {
        auto a = pass(target, true);
        auto b = pass(target, false);
        compile({a, between(), b});
        check(a->colorArrayOps()[0]->store, "a later pass that LOADS the target makes the earlier one store colour");
        check(a->depthStencilOps()->storeDepth, "and depth");
    }
    {
        auto a = pass(target, true);
        auto b = pass(target, true);
        compile({a, b});
        check(!a->colorArrayOps()[0]->store && !a->depthStencilOps()->storeDepth,
            "a later pass that CLEARS it does not");
    }
    {
        auto a = pass(nullptr, true);
        auto b = pass(nullptr, false);
        compile({a, between(), b});
        check(a->colorArrayOps()[0]->store && a->depthStencilOps()->storeDepth,
            "the back buffer (null target) takes part like any other target");
    }
    {
        auto a = pass(nullptr, true);
        auto grab = std::make_shared<RenderPass>(device);   // never initialised: no attachment ops, as a grab
        auto b = pass(nullptr, false);
        compile({a, grab, b});
        check(a->colorArrayOps()[0]->store, "a grab between them does not displace the pass that filled the surface");
    }

    std::cout << "\nmerging and before-passes\n";
    {
        auto a = pass(target, true);
        auto b = pass(target, false);
        compile({a, b});
        check(a->skipEnd() && b->skipStart(), "two adjacent passes on one target, the second loading, merge");
        auto c = pass(target, true);
        compile({a, c});
        check(!a->skipEnd() && !c->skipStart(), "a clearing second pass does not, and last frame's merge is reset");
    }
    {
        // A camera frame's compose and its after pass (the UI layer): both on the back
        // buffer, the second loading.
        auto compose = pass(nullptr, true);
        auto ui = pass(nullptr, false);
        compile({compose, ui});
        check(compose->skipEnd() && ui->skipStart(), "two back-buffer passes merge (null is the back buffer)");
        check(!compose->colorArrayOps()[0]->store && !compose->depthStencilOps()->storeDepth,
            "the merged pass ends with the LAST pass's stores, not the one raised for the load it absorbed");
        auto grab = std::make_shared<RenderPass>(device);   // no target: never merges
        auto b = pass(nullptr, false);
        compile({compose, grab, b});
        check(!compose->skipEnd() && !grab->skipStart() && !grab->skipEnd() && !b->skipStart(),
            "a pass with no target between them merges with neither");
        check(compose->colorArrayOps()[0]->store, "and the store the next frame's adjacency needs is back");
    }
    {
        // A chain of three takes the third pass's end, and a store the last pass needs
        // (a later pass reads the target) reaches the first.
        auto a = pass(target, true);
        auto b = pass(target, false);
        auto c = pass(target, false);
        auto other = pass(nullptr, true);
        auto reader = pass(target, false);
        compile({a, b, c, other, reader});
        check(a->skipEnd() && b->skipStart() && b->skipEnd() && c->skipStart(), "three adjacent passes chain");
        check(a->colorArrayOps()[0]->store && a->depthStencilOps()->storeDepth,
            "the chain ends storing what a later reader loads");
    }
    {
        auto child = pass(target, true);
        auto parent = pass(target, false);
        parent->addBeforePass(child);
        compile({parent});
        // Store propagation says who ran first: the loading parent raises the store of
        // the pass BEFORE it. (A pass with before-passes never merges, by design.)
        check(child->colorArrayOps()[0]->store && !parent->skipStart(),
            "a before-pass runs BEFORE its parent, and the parent does not merge into it");
        auto disabled = pass(target, true);
        disabled->setEnabled(false);
        auto parent2 = pass(target, false);
        parent2->addBeforePass(disabled);
        compile({parent2});
        check(!disabled->colorArrayOps()[0]->store, "a disabled before-pass is left out");
    }

    std::cout << "\nscheduling and disabled passes\n";
    {
        std::vector<std::string> log;
        auto main = std::make_shared<LoggingPass>(device, "main", log);
        auto before = std::make_shared<LoggingPass>(device, "before", log);
        auto after = std::make_shared<LoggingPass>(device, "after", log);
        main->addBeforePass(before);
        main->addAfterPass(after);
        compile({main});
        check(joined(log) == "main before after", "a pass is updated, then its before-passes and after-passes in order");

        log.clear();
        main->setEnabled(false);
        compile({main});
        check(log.empty(), "a disabled pass is skipped with its before and after passes, and not updated");
        main->setEnabled(true);

        log.clear();
        auto parent = std::make_shared<LoggingPass>(device, "parent", log);
        auto child = std::make_shared<LoggingPass>(device, "child", log);
        auto grandBefore = std::make_shared<LoggingPass>(device, "grandBefore", log);
        auto grandAfter = std::make_shared<LoggingPass>(device, "grandAfter", log);
        child->addBeforePass(grandBefore);
        child->addAfterPass(grandAfter);
        child->setEnabled(false);
        parent->addBeforePass(child);
        compile({parent});
        check(joined(log) == "parent", "a disabled child is skipped together with its own before and after passes");
    }
    {
        auto a = pass(target, true);
        auto b = pass(otherTarget, false);
        compile({a, b});
        check(!a->skipEnd() && !b->skipStart(), "passes on different targets do not merge");

        auto c = pass(target, true);
        auto depthClear = pass(target, false);
        depthClear->depthStencilOps()->clearDepth = true;
        compile({c, depthClear});
        check(!c->skipEnd() && !depthClear->skipStart(), "a second pass clearing only depth does not merge");
        auto stencilClear = pass(target, false);
        stencilClear->depthStencilOps()->clearStencil = true;
        compile({c, stencilClear});
        check(!c->skipEnd() && !stencilClear->skipStart(), "nor one clearing only stencil");

        auto owner = pass(target, true);
        auto ownAfter = pass(target, false);
        owner->addAfterPass(ownAfter);
        compile({owner});
        check(!owner->skipEnd() && !ownAfter->skipStart(), "a pass does not merge with its own after-pass");
        auto ownBefore = pass(target, true);
        auto owner2 = pass(target, false);
        owner2->addBeforePass(ownBefore);
        compile({owner2});
        check(!ownBefore->skipEnd() && !owner2->skipStart(), "nor with its own before-pass");
    }
    {
        // Each attachment's store follows whether the next pass on the target LOADS it.
        auto a = pass(target, true);
        auto b = pass(target, true);
        b->depthStencilOps()->clearDepth = false;   // loads depth, clears colour
        compile({a, between(), b});
        check(!a->colorArrayOps()[0]->store && a->depthStencilOps()->storeDepth,
            "a later pass loading only depth makes the earlier one store depth alone");
        auto c = pass(target, true);
        auto d = pass(target, true);
        d->colorArrayOps()[0]->clear = false;   // loads colour, clears depth
        compile({c, between(), d});
        check(c->colorArrayOps()[0]->store && !c->depthStencilOps()->storeDepth,
            "and one loading only colour, colour alone");
    }

    std::cout << "\nN13: the graph's edits last one frame\n";
    {
        auto a = pass(target, true);
        auto b = pass(target, false);
        auto c = pass(target, true);
        compile({a, between(), b});
        check(a->colorArrayOps()[0]->store, "frame 1: B loads the target, so A stores");
        compile({a, c});
        check(!a->colorArrayOps()[0]->store && !a->depthStencilOps()->storeDepth,
            "frame 2: C clears it, so A's store goes back to its own value");
        compile({a, between(), b});
        check(a->colorArrayOps()[0]->store, "frame 3: B again, A stores again");
        a->depthStencilOps()->storeDepth = false;   // the pass changes its own flag...
        compile({a, c});
        a->depthStencilOps()->storeDepth = true;    // ...and between frames sets it itself
        compile({a, c});
        check(a->depthStencilOps()->storeDepth, "a flag the pass set itself is not undone by the graph");
    }
    {
        TextureOptions cubeOptions;
        cubeOptions.width = 16;
        cubeOptions.height = 16;
        cubeOptions.cubemap = true;
        Texture cube(device.get(), cubeOptions);
        auto face0 = pass(makeTarget(&cube, 0), true);
        auto face1 = pass(makeTarget(&cube, 1), true);
        face0->colorArrayOps()[0]->genMipmaps = true;
        face1->colorArrayOps()[0]->genMipmaps = true;
        compile({face0, face1});
        check(!face0->colorArrayOps()[0]->genMipmaps && face1->colorArrayOps()[0]->genMipmaps,
            "two faces of one cubemap: only the LAST generates mipmaps");
        compile({face0});
        check(face0->colorArrayOps()[0]->genMipmaps, "a frame with one face gives its mipmap generation back");
    }

    std::cout << "onBefore / onAfter\n";
    {
        // Merged with its neighbour, a pass's callback would run inside the shared device pass.
        for (const bool onAfterFirst : {true, false}) {
            auto first = pass(target, true);
            auto second = pass(target, false);
            if (onAfterFirst) {
                first->setOnAfter([](RenderPass&) {});
            } else {
                second->setOnBefore([](RenderPass&) {});
            }
            compile({first, second});
            check(!first->skipEnd() && !second->skipStart(), onAfterFirst
                ? "the first pass's onAfter keeps the two passes apart"
                : "the second pass's onBefore keeps the two passes apart");
        }
        auto first = pass(target, true);
        auto second = pass(target, false);
        first->setOnBefore([](RenderPass&) {});
        second->setOnAfter([](RenderPass&) {});
        compile({first, second});
        check(first->skipEnd() && second->skipStart(), "callbacks that do not run between them leave the merge alone");

        auto idle = pass(target, true);
        auto next = pass(target, false);
        idle->setExecuteEnabled(false);
        compile({idle, next});
        check(!idle->skipEnd() && !next->skipStart(), "a pass that does not execute is not merged");
    }
    {
        const auto recording = std::make_shared<StubGraphicsDevice>(
            StubGraphicsDevice::Options{.size = {64, 64}, .recordDraws = true});
        RenderTargetOptions options;
        options.graphicsDevice = recording.get();
        options.colorBuffer = &colour;
        const auto recordingTarget = std::make_shared<StubRenderTarget>(options);

        std::vector<std::string> log;
        RecordingPass p(recording, log);
        p.init(recordingTarget);
        RenderPass* seen = nullptr;
        p.setOnBefore([&](RenderPass& self) {
            seen = &self;
            log.push_back(recording->insideRenderPass() ? "onBefore(inside)" : "onBefore");
        });
        p.setOnAfter([&](RenderPass& self) {
            check(&self == seen, "both callbacks are handed the pass itself");
            log.push_back(recording->insideRenderPass() ? "onAfter(inside)" : "onAfter");
        });
        p.render();
        check(joined(log) == "before onBefore execute after onAfter",
            "onBefore after before(), onAfter after after(), both outside the device pass: " + joined(log));
        check(seen == &p, "onBefore is handed the pass");

        log.clear();
        p.setExecuteEnabled(false);
        p.render();
        check(joined(log) == "before onBefore after onAfter", "they run when execute() is disabled: " + joined(log));

        log.clear();
        p.setEnabled(false);
        p.render();
        check(log.empty(), "and not when the pass is: " + joined(log));
    }

    std::cout << "\nComputePass\n";
    {
        std::vector<std::string> log;
        const auto logging = std::make_shared<DispatchLoggingDevice>(log);
        Compute first(logging.get(), nullptr, "First");
        Compute second(logging.get(), nullptr, "Second");
        ComputePass computePass(logging, {&first, &second});
        computePass.setName("TestPass");
        computePass.render();
        check(joined(log) == "dispatch[TestPass First Second]",
            "it dispatches its computes in order, in one dispatch named after the pass: " + joined(log));

        log.clear();
        ComputePass empty(logging);
        empty.render();
        check(log.empty(), "with no computes it dispatches nothing");

        log.clear();
        computePass.setEnabled(false);
        computePass.render();
        check(log.empty(), "nor when it is disabled");
    }
    {
        // Between two passes on one target, the second loading: they would merge.
        const auto between = std::make_shared<ComputePass>(device);
        auto a = pass(target, true);
        auto b = pass(target, false);
        compile({a, between, b});
        check(!a->skipEnd() && !between->skipStart() && !between->skipEnd() && !b->skipStart(),
            "a compute pass keeps the passes around it apart, and merges with neither");
        check(a->colorArrayOps()[0]->store && a->depthStencilOps()->storeDepth,
            "and does not displace the first as the pass the second loads from, which stores");
        check(!between->hasRenderTarget(), "it has no target, so it starts no device pass");
    }
    {
        // Rendered: the dispatch lands between the two device passes, outside both.
        std::vector<std::string> log;
        const auto logging = std::make_shared<DispatchLoggingDevice>(log);
        RenderTargetOptions options;
        options.graphicsDevice = logging.get();
        options.colorBuffer = &colour;
        const auto loggingTarget = std::make_shared<StubRenderTarget>(options);
        const auto makePass = [&](const bool clear) {
            auto p = std::make_shared<RenderPass>(logging);
            p->init(loggingTarget);
            p->colorArrayOps()[0]->clear = clear;
            p->depthStencilOps()->clearDepth = clear;
            return p;
        };
        Compute kernel(logging.get(), nullptr, "Kernel");
        const auto computePass = std::make_shared<ComputePass>(logging, std::vector<Compute*>{&kernel});
        computePass->setName("Mid");
        bool onBeforeOutside = false;
        computePass->setOnBefore([&](RenderPass&) { onBeforeOutside = !logging->insideRenderPass(); });
        FrameGraph graph;
        graph.addRenderPass(makePass(true));
        graph.addRenderPass(computePass);
        graph.addRenderPass(makePass(false));
        graph.render(logging.get());
        check(joined(log) == "start end dispatch[Mid Kernel] start end",
            "in a rendered graph the dispatch runs after the pass before it ends and before the next starts: "
                + joined(log));
        check(onBeforeOutside, "its onBefore runs outside any device pass, to set the dispatch up");
    }

    std::cout << "\ncamera after-passes\n";
    {
        const auto engineDevice = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{
            .size = {64, 48}, .cpuBuffers = true, .recordDraws = true, .renderTargets = true});
        auto engine = makeTestEngine<RenderComponentSystem, CameraComponentSystem>(engineDevice);

        Entity* firstEntity = addTo(engine->root(), newEntity(engine.get(), "First"));
        firstEntity->setLocalPosition(0.0f, 0.0f, 8.0f);
        auto* firstCamera = static_cast<CameraComponent*>(firstEntity->addComponent<CameraComponent>());
        Entity* secondEntity = addTo(engine->root(), newEntity(engine.get(), "Second"));
        secondEntity->setLocalPosition(8.0f, 0.0f, 0.0f);
        auto* secondCamera = static_cast<CameraComponent*>(secondEntity->addComponent<CameraComponent>());
        secondCamera->setPriority(1);

        auto material = std::make_shared<StandardMaterial>();
        Entity* box = addTo(engine->root(), newEntity(engine.get(), "Box"));
        auto* render = static_cast<RenderComponent*>(box->addComponent<RenderComponent>());
        render->setMaterial(material.get());
        render->setType("box");

        engine->start();
        engine->update(1.0f / 60.0f);
        engine->render();

        const auto buildPasses = [&] {
            engine->scene()->layers()->markDirty();
            FrameGraph frameGraph;
            engine->renderer()->buildFrameGraph(&frameGraph, engine->scene()->layers().get());
            return frameGraph.renderPasses();
        };
        const auto rendersCamera = [](const std::shared_ptr<RenderPass>& p, const CameraComponent* camera) {
            const auto* forward = dynamic_cast<const RenderPassForward*>(p.get());
            return forward && forward->camera() == camera;
        };
        using Passes = std::vector<std::shared_ptr<RenderPass>>;
        const auto indexOf = [](const Passes& passes, const RenderPass* p) {
            const auto it = std::find_if(passes.begin(), passes.end(), [p](const auto& e) { return e.get() == p; });
            return it == passes.end() ? -1 : static_cast<int>(it - passes.begin());
        };
        const auto lastIndex = [&](const Passes& passes, const CameraComponent* camera) {
            int last = -1;
            for (int i = 0; i < static_cast<int>(passes.size()); ++i) {
                last = rendersCamera(passes[i], camera) ? i : last;
            }
            return last;
        };
        const auto firstIndex = [&](const Passes& passes, const CameraComponent* camera) {
            for (int i = 0; i < static_cast<int>(passes.size()); ++i) {
                if (rendersCamera(passes[i], camera)) {
                    return i;
                }
            }
            return -1;
        };

        check(&firstCamera->afterPasses() == &firstCamera->camera()->afterPasses(),
            "the component's after-passes are its camera's");

        const auto after = std::make_shared<ComputePass>(engineDevice);
        firstCamera->afterPasses().push_back(after);
        {
            const Passes passes = buildPasses();
            const int index = indexOf(passes, after.get());
            check(index > lastIndex(passes, firstCamera) && lastIndex(passes, firstCamera) >= 0,
                "an after-pass runs after the camera renders");
            check(index >= 0 && index < firstIndex(passes, secondCamera),
                "and before the camera rendering after it");
        }

        firstCamera->requestSceneColorMap(true);
        firstCamera->requestSceneDepthMap(true);
        {
            const Passes passes = buildPasses();
            const int index = indexOf(passes, after.get());
            int lastGrab = -1;
            for (int i = 0; i < static_cast<int>(passes.size()); ++i) {
                if (dynamic_cast<RenderPassColorGrab*>(passes[i].get()) ||
                    dynamic_cast<RenderPassDepthGrab*>(passes[i].get())) {
                    lastGrab = i;
                }
            }
            check(lastGrab >= 0 && index > lastGrab, "and after the camera's scene maps are grabbed");
            check(index > lastIndex(passes, firstCamera) && index < firstIndex(passes, secondCamera),
                "still between the two cameras");
        }
        firstCamera->requestSceneColorMap(false);
        firstCamera->requestSceneDepthMap(false);

        const auto ownFirst = std::make_shared<RenderPass>(engineDevice);
        const auto ownSecond = std::make_shared<RenderPass>(engineDevice);
        firstCamera->setRenderPasses({ownFirst, ownSecond});
        {
            const Passes passes = buildPasses();
            const int index = indexOf(passes, after.get());
            check(index >= 0 && index == indexOf(passes, ownSecond.get()) + 1,
                "a camera rendering through its own passes runs its after-passes right after them");
            check(index < firstIndex(passes, secondCamera), "and before the next camera");
        }
        firstCamera->setRenderPasses({});

        after->setEnabled(false);
        {
            const Passes passes = buildPasses();
            check(indexOf(passes, after.get()) < 0, "a disabled after-pass is left out");
        }
        firstCamera->afterPasses().clear();
    }

    return finish("frame graph");
}
