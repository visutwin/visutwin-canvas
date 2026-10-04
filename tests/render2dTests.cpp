// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.10.2026
//
// Render2d, the performance HUD's quad list, on a stub device.
//
// What a render cannot show: a quad's texture rectangle measured from the wrong corner
// draws a glyph upside down only on the glyphs that are not symmetric; a clip that cuts
// the position but not the texture coordinates squashes the glyph instead of cutting it;
// a list committed every frame costs an arena block per frame and looks the same. And
// the draw-once gate: two cameras rendering the UI layer draw the overlay twice, on top
// of itself, which looks the same as once.

#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/applicationStats.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/extras/miniStats/render2d.h"
#include "platform/graphics/texture.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/scene.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr int kStride = static_cast<int>(UiGeometryArena::kFloatsPerVertex);

    // Field of vertex `vertex` of quad `quad`.
    float field(const Render2d& r, const int quad, const int vertex, const int offset)
    {
        return r.vertices()[static_cast<size_t>((quad * 4 + vertex) * kStride + offset)];
    }

    // The order commit() writes: bottom-left, bottom-right, top-right, top-left.
    enum Corner { BL = 0, BR = 1, TR = 2, TL = 3 };
    enum Field { X = 0, Y = 1, MODE = 2, R = 3, G = 4, B = 5, U = 6, V = 7, INVH = 8, FRAC = 9, A = 10 };

    Entity* addCamera(Engine& engine, const std::string& name, const int priority)
    {
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        entity->setEngine(&engine);
        entity->setName(name);
        engine.root()->addChild(std::move(owned));
        auto* camera = static_cast<CameraComponent*>(entity->addComponent<CameraComponent>());
        camera->setPriority(priority);
        return entity;
    }
}

int main()
{
    std::cout << std::unitbuf;

    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{
        .size = {200, 100}, .cpuBuffers = true, .recordDraws = true, .renderTargets = true});

    std::cout << "quad packing\n";
    {
        Render2d r(device);
        r.setTargetSize(200.0f, 100.0f);
        r.startFrame();
        // A glyph-like source rectangle: texels (10, 20) to (30, 60) of a 100 x 200 texture.
        check(r.quad(20.0f, 10.0f, 40.0f, 20.0f, 10.0f, 20.0f, 20.0f, 40.0f, 100.0f, 200.0f,
                     Render2d::Mode::Text, 0x80402010u) == 0, "the first quad is index 0");
        r.render(nullptr);
        check(r.vertices().size() == 4u * kStride, "four vertices of 14 floats");
        check(near(field(r, 0, BL, X), 0.1f, 1e-6f) && near(field(r, 0, BL, Y), 0.1f, 1e-6f) &&
              near(field(r, 0, TR, X), 0.3f, 1e-6f) && near(field(r, 0, TR, Y), 0.3f, 1e-6f),
              "positions are fractions of the target from its bottom-left corner");
        check(field(r, 0, BL, MODE) == 1.0f, "the mode rides in position.z");
        check(near(field(r, 0, BL, R), 0x10 / 255.0f, 1e-6f) && near(field(r, 0, BL, G), 0x20 / 255.0f, 1e-6f) &&
              near(field(r, 0, BL, B), 0x40 / 255.0f, 1e-6f) && near(field(r, 0, BL, A), 0x80 / 255.0f, 1e-6f),
              "colour 0xAABBGGRR unpacks to rgb in the normal and alpha in tangent.z");
        // v = 0 is the texture's top row, so the quad's TOP edge samples the rectangle's top row.
        check(near(field(r, 0, TL, U), 0.1f, 1e-6f) && near(field(r, 0, TL, V), 0.1f, 1e-6f),
              "top-left corner samples texel (10, 20): the rectangle's top-left");
        check(near(field(r, 0, BR, U), 0.3f, 1e-6f) && near(field(r, 0, BR, V), 0.3f, 1e-6f),
              "bottom-right corner samples texel (30, 60): the rectangle's bottom-right");
        check(near(field(r, 0, BL, INVH), 1.0f / 20.0f, 1e-6f) && field(r, 0, BL, FRAC) == 0.0f &&
              field(r, 0, TL, FRAC) == 1.0f, "tangent.xy carries 1 / height and the height fraction");
    }

    std::cout << "\nclipping\n";
    {
        Render2d r(device);
        r.setTargetSize(200.0f, 100.0f);
        r.startFrame();
        r.setClip(0.0f, 15.0f, 40.0f, 100.0f);
        // Cut on the right at x = 40 (half the width) and at the bottom at y = 15 (a quarter).
        r.quad(20.0f, 10.0f, 40.0f, 20.0f, 0.0f, 0.0f, 40.0f, 20.0f, 40.0f, 20.0f, Render2d::Mode::Text, 0xffffffffu);
        check(r.quad(50.0f, 10.0f, 10.0f, 10.0f, 0, 0, 0, 0, 1, 1, Render2d::Mode::Solid, 0xffffffffu) == -1,
              "a quad wholly outside the clip is not added");
        r.render(nullptr);
        check(near(field(r, 0, BR, X), 0.2f, 1e-6f) && near(field(r, 0, BL, Y), 0.15f, 1e-6f),
              "the position is cut to the clip");
        check(near(field(r, 0, BR, U), 0.5f, 1e-6f), "u is cut with it: half the width, half the texture");
        check(near(field(r, 0, BL, V), 0.75f, 1e-6f) && near(field(r, 0, TL, V), 0.0f, 1e-6f),
              "v is cut with it, from the bottom: a quarter off the bottom row of the texture");
        check(near(field(r, 0, BL, FRAC), 0.25f, 1e-6f), "the height fraction starts where the clip cut");
    }

    std::cout << "\ngraph quads\n";
    {
        Render2d r(device);
        r.setTargetSize(200.0f, 100.0f);
        r.startFrame();
        r.graph(8.0f, 8.0f, 100.0f, 20.0f, 3, 256.0f, 8.0f, 0xff6db1d9u);
        r.render(nullptr);
        check(near(field(r, 0, BL, U), -100.0f / 256.0f, 1e-6f) && near(field(r, 0, BR, U), 0.0f, 1e-6f),
              "columns [cursor - width, cursor): u from -width to 0, the cursor added by the shader");
        check(near(field(r, 0, BL, V), 3.5f / 8.0f, 1e-6f) && near(field(r, 0, TL, V), 3.5f / 8.0f, 1e-6f),
              "one row of the history texture, sampled through its texel centres");
        check(field(r, 0, BL, MODE) == 2.0f, "graph mode");
        r.setGraphCursor(64.0f, 256.0f);
        r.render(nullptr);
        check(r.material()->params()[0] == 0.25f, "the cursor reaches the uniform block in texture widths");
    }

    std::cout << "\ncommitting\n";
    {
        Render2d r(device);
        r.setTargetSize(200.0f, 100.0f);
        r.startFrame();
        r.rect(0.0f, 0.0f, 10.0f, 10.0f, 0xffffffffu);
        r.render(nullptr);
        const MeshInstance* instance = r.meshInstance();
        const int firstBase = instance->mesh()->getPrimitive().base;
        check(instance->visible() && instance->mesh()->getPrimitive().count == 6, "one quad: six indices, visible");
        check(instance->screenSpace() && instance->drawOncePerFrame() && !instance->castShadow(),
              "screen space, drawn once a frame, no shadow");

        const uint64_t version = r.material()->uniformsVersion();
        for (int frame = 0; frame < 5; ++frame) {
            r.render(nullptr);
        }
        check(instance->mesh()->getPrimitive().base == firstBase, "unchanged, it is not committed again");
        check(r.material()->uniformsVersion() == version, "and the material's version does not move");

        r.startFrame();
        r.rect(0.0f, 0.0f, 10.0f, 10.0f, 0xffffffffu);
        r.render(nullptr);
        check(instance->mesh()->getPrimitive().base != firstBase,
              "a new list takes a new block: the old one may still be read by a frame in flight");

        r.startFrame();
        r.render(nullptr);
        check(!instance->visible(), "an empty list hides the instance rather than draw nothing");
    }

    std::cout << "\nmaterial\n";
    {
        Render2d r(device);
        Texture regular(device.get());
        Texture bold(device.get());
        Texture graph(device.get());
        const uint64_t before = r.material()->uniformsVersion();
        r.setTextures(&regular, &bold, &graph);
        std::vector<TextureSlot> slots;
        r.material()->getTextureSlots(slots);
        check(slots.size() == 3 && slots[0].slot == 0 && slots[0].texture == &regular &&
              slots[1].slot == 1 && slots[1].texture == &bold && slots[2].slot == 3 && slots[2].texture == &graph,
              "font pages at slots 0 and 1, the graph at 3");
        check(r.material()->uniformsVersion() != before,
              "a texture change moves the version, so neither backend keeps the old binding");
        size_t size = 0;
        r.material()->customUniformData(size);
        check(size == 32, "the uniform block is two vec4s");
        check(r.material()->transparent() && !r.material()->depthState()->depthTest() &&
              !r.material()->depthState()->depthWrite(), "blended, no depth test, no depth write");
    }

    std::cout << "\ndrawn once a frame, whatever renders its layer\n";
    {
        auto engine = makeTestEngine<CameraComponentSystem>(device);
        addCamera(*engine, "Camera", 0);
        Entity* second = addCamera(*engine, "Camera2", 1);
        Layer* ui = engine->scene()->layers()->getLayerById(LAYERID_UI).get();
        check(ui != nullptr, "the scene has a UI layer");

        Render2d r(device);
        r.setTargetSize(200.0f, 100.0f);
        r.startFrame();
        r.rect(8.0f, 8.0f, 50.0f, 20.0f, 0xff231b15u);
        r.render(ui);

        engine->start();
        auto& stats = const_cast<ApplicationStats&>(*engine->stats());
        for (int frame = 1; frame <= 3; ++frame) {
            engine->update(1.0f / 60.0f);
            r.render(ui);
            engine->render();
            check(stats.frame().cameras == 2 && stats.drawCalls().forward == 1,
                  "frame " + std::to_string(frame) + ": two cameras render the UI layer, one draw");
        }

        r.meshInstance()->setDrawOncePerFrame(false);
        engine->update(1.0f / 60.0f);
        engine->render();
        check(stats.drawCalls().forward == 2, "without the flag each camera draws it: the control");
        r.meshInstance()->setDrawOncePerFrame(true);

        second->setEnabled(false);
        engine->update(1.0f / 60.0f);
        engine->render();
        check(stats.drawCalls().forward == 1, "one camera: still drawn");

        r.setLayer(nullptr);
        engine->update(1.0f / 60.0f);
        engine->render();
        check(stats.drawCalls().forward == 0, "taken off the layer: not drawn");
    }

    return finish("render2d");
}
