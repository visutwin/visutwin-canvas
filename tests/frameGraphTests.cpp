// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// FrameGraph::compile on a stub device: the load/store contract it derives between
// passes that share a target, pass merging, before-pass ordering, and — since
// 2026-09-25 — that its edits to a PERSISTENT pass's attachment flags last one frame.
//
// Passes persist across frames while the graph is rebuilt every frame. compile() used
// to raise a store or drop a cubemap mip generation and never put it back, so one
// frame's adjacency stuck to a pass for good (audit 2026-09-23 renderer N13). It now
// records each edit and undoes it at the next compile, leaving alone any flag the pass
// changed itself.

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/frameGraph.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    class StubDevice final : public GraphicsDevice
    {
    public:
        void draw(const Primitive&, const std::shared_ptr<IndexBuffer>&, int, int, bool, bool) override {}
        void startRenderPass(RenderPass*) override {}
        void endRenderPass(RenderPass*) override {}
        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture*) override { return nullptr; }
        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>&, int,
            const VertexBufferOptions&) override { return nullptr; }
        std::shared_ptr<IndexBuffer> createIndexBuffer(IndexFormat, int, const std::vector<uint8_t>&) override
        {
            return nullptr;
        }
        void setResolution(int, int) override {}
        std::pair<int, int> size() const override { return {64, 64}; }
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions&) override { return nullptr; }
    };

    std::shared_ptr<GraphicsDevice> device;

    class StubRenderTarget final : public RenderTarget
    {
    public:
        using RenderTarget::RenderTarget;
    protected:
        void destroyFrameBuffers() override {}
        void createFrameBuffers() override {}
    };

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
    device = std::make_shared<StubDevice>();

    TextureOptions colourOptions;
    colourOptions.width = 64;
    colourOptions.height = 64;
    colourOptions.mipmaps = false;
    Texture colour(device.get(), colourOptions);
    const auto target = makeTarget(&colour);

    std::cout << "store propagation\n";
    {
        auto a = pass(target, true);
        auto b = pass(target, false);
        compile({a, b});
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
        compile({a, b});
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

    std::cout << "\nN13: the graph's edits last one frame\n";
    {
        auto a = pass(target, true);
        auto b = pass(target, false);
        auto c = pass(target, true);
        compile({a, b});
        check(a->colorArrayOps()[0]->store, "frame 1: B loads the target, so A stores");
        compile({a, c});
        check(!a->colorArrayOps()[0]->store && !a->depthStencilOps()->storeDepth,
            "frame 2: C clears it, so A's store goes back to its own value");
        compile({a, b});
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

    std::cout << (failures == 0 ? "\nAll frame graph tests passed\n" : "\nFrame graph tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
