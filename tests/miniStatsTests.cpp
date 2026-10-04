// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.10.2026
//
// The performance HUD on a stub device: the graph rows' averaging and history (whose
// oracle is upstream's Graph), the history texture ring, and the panel itself — row order,
// panel height, size cycling, collapsing, the pointer, resource counts and the one draw.
//
// Nothing here is visible as an error in a render: an average taken over the wrong
// window reads as a plausible figure, a count row whose scale grows without rescaling
// what it holds reads as resources being freed, and a history ring of one texture tears
// on Metal only under load.

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/applicationStats.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/extras/miniStats/miniStats.h"
#include "platform/graphics/texture.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/scene.h"
#include "support/check.h"
#include "support/msdfTestFont.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    // A device that keeps three frames in flight, as Metal does.
    class ThreeFrameDevice final : public StubGraphicsDevice
    {
    public:
        using StubGraphicsDevice::StubGraphicsDevice;
        int maxFramesInFlight() const override { return 3; }
    };

    SDL_Event mouseButton(const bool down, const float x, const float yFromTop)
    {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.x = x;
        e.button.y = yFromTop;
        return e;
    }

    SDL_Event mouseMotion(const float x, const float yFromTop)
    {
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.x = x;
        e.motion.y = yFromTop;
        return e;
    }

    SDL_Event wheel(const float x, const float yFromTop, const float notches)
    {
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.mouse_x = x;
        e.wheel.mouse_y = yFromTop;
        e.wheel.y = notches;
        return e;
    }

    std::vector<std::string> visibleLabels(const MiniStats& stats)
    {
        // A row a rebuild placed on screen has its heading rectangle or its quad... the
        // simplest outside view is the display order, filtered as the panel filters it.
        std::vector<std::string> out;
        for (const auto& g : stats.graphs()) {
            out.push_back(g->label);
        }
        return out;
    }
}

int main()
{
    std::cout << std::unitbuf;

    std::cout << "a graph row\n";
    {
        float value = 0.0f;
        MiniStatsGraph graph("CPU", 33.0f, 500.0f, [&value]() { return value; }, 1, "ms");
        MiniStatsHistory history(nullptr);
        history.ensureSize(256, 1);
        graph.row = 0;
        graph.enabled = true;
        int changed = 0;
        const float samples[] = {10.0f, 20.0f, 30.0f, 40.0f, 50.0f};
        for (const float sample : samples) {
            value = sample;
            changed = graph.update(100.0f, &history);
            history.advance();
        }
        check(graph.timingText == "30.0" && graph.maxText == "50.0" && changed == 3,
              "after 500 ms: the mean and peak of the window, both texts changed");
        value = 99.0f;
        check(graph.update(100.0f, &history) == 0 && graph.timingText == "30.0",
              "a new window starts: the text holds until it is full");
        const uint8_t* row = history.row(0);
        // Range 1.5 x 33 = 49.5; 10 / 49.5 x 255 = 51.5 -> 52; 50 / 49.5 clamps to 255.
        check(row[0] == 52 && row[3] == 170 && row[4 * 4] == 255 && row[5 * 4] == 255,
              "history columns: the value over 1.5 budgets, alpha 170 (the budget line at 1/1.5)");
        check(row[6 * 4 + 3] == 0, "an unwritten column holds no sample");
    }

    std::cout << "\na count row\n";
    {
        MiniStatsGraph graph("Textures", 0.0f, 500.0f, nullptr);
        graph.countOnly = true;
        graph.enabled = true;
        graph.row = 0;
        MiniStatsHistory history(nullptr);
        history.ensureSize(256, 1);
        graph.count = 10;
        graph.update(16.0f, &history);
        history.advance();
        check(graph.historyRange == 16.0f && history.row(0)[0] == 159 && history.row(0)[3] == 255,
              "the scale starts at 16: 10 / 16 x 255 = 159, alpha 255 (no budget)");
        graph.count = 40;
        graph.update(16.0f, &history);
        check(graph.historyRange == 64.0f, "40 grows the scale to the next power of two");
        check(history.row(0)[0] == 40 && history.row(0)[4] == 159,
              "and what the row held is rescaled (159 x 16 / 64 = 40): no false drop");
    }

    std::cout << "\nthe history texture\n";
    {
        auto device = std::make_shared<ThreeFrameDevice>(StubGraphicsDevice::Options{});
        MiniStatsHistory history(device.get());
        history.ensureSize(200, 3);
        check(history.width() == 256 && history.height() == 4, "powers of two");
        history.row(2)[0] = 77;
        history.ensureSize(200, 5);
        check(history.height() == 8 && history.row(2)[0] == 77, "growing keeps every row's samples");
        Texture* a = history.upload();
        Texture* b = history.upload();
        Texture* c = history.upload();
        Texture* d = history.upload();
        check(a != b && b != c && a != c && d == a,
              "a ring of maxFramesInFlight textures: a texture is rewritten every third frame");
        check(history.current() == a && a->width() == 256 && a->height() == 8, "sized to the rows");
        for (int i = 0; i < 256; ++i) {
            history.advance();
        }
        check(history.cursor() == 0, "the cursor wraps at the width");
    }

    std::cout << "\nthe panel\n";
    {
        auto device = std::make_shared<ThreeFrameDevice>(StubGraphicsDevice::Options{
            .size = {640, 480}, .cpuBuffers = true, .recordDraws = true, .renderTargets = true});
        auto engine = makeTestEngine<CameraComponentSystem>(device);
        {
            auto owned = std::make_unique<Entity>();
            owned->setEngine(engine.get());
            owned->addComponent<CameraComponent>();
            engine->root()->addChild(std::move(owned));
        }
        auto regular = makeMsdfTestFont(device.get());
        auto bold = makeMsdfTestFont(device.get());
        MiniStatsOptions options;
        options.regularFont = regular.get();
        options.boldFont = bold.get();
        auto stats = std::make_unique<MiniStats>(engine, options);
        check(stats->textValid(), "the fonts are usable");
        engine->start();
        const auto frame = [&engine] {
            engine->update(1.0f / 60.0f);
            engine->render();
        };
        frame();

        const std::vector<std::string> order = visibleLabels(*stats);
        check(order.size() >= 6 && order[0] == "Engine" && order[1] == "Draw calls" && order[2] == "Frame" &&
              order[3] == "CPU" && order[4] == "GPU" && order[5] == "VRAM" && order[6] == "Resources",
              "Engine first (draw calls, frame), then CPU, GPU, VRAM, Resources");

        check(stats->activeSizeIndex() == 0 && stats->overallHeight() == 8.0f + 5 * 22.0f,
              "compact: five rows of 22 and 8 of padding, no headings");
        check(stats->panelWidth() == 128.0f && stats->panelHeight() == 118.0f, "128 wide");

        auto& appStats = const_cast<ApplicationStats&>(*engine->stats());
        check(appStats.drawCalls().forward == 1, "drawn by the camera rendering the UI layer, one draw");

        // The panel's bottom-left corner is 8 points in; SDL measures y from the top.
        const float h = 480.0f;
        const auto clickAt = [&](const float x, const float yUp) {
            const bool down = stats->handleEvent(mouseButton(true, x, h - yUp));
            const bool up = stats->handleEvent(mouseButton(false, x, h - yUp));
            return down && up;
        };

        check(!stats->handleEvent(mouseButton(true, 300.0f, h - 50.0f)), "a press off the panel is not taken");
        check(clickAt(20.0f, 20.0f) && stats->activeSizeIndex() == 1, "a click on the panel: the next size");
        frame();
        // Detailed: Engine(3 rows) | CPU + Render + Update | GPU | VRAM + 3 parts | Resources (collapsed).
        const float expected = 31.0f + 12 * 20.0f + 4 * 5.0f;
        check(stats->overallHeight() == expected,
              "detailed: 31 of padding, 20 per row, 5 between sections (" +
              std::to_string(stats->overallHeight()) + " vs " + std::to_string(expected) + ")");

        // The Engine heading is the first row under the column labels.
        const MiniStatsGraph* engineHeader = stats->graphs()[0].get();
        check(engineHeader->headerTop > engineHeader->headerBottom, "the heading's rectangle is recorded");
        const float headingY = (engineHeader->headerTop + engineHeader->headerBottom) * 0.5f;
        check(clickAt(20.0f, headingY) && stats->groupCollapsed(MiniStats::ENGINE) && stats->activeSizeIndex() == 1,
              "a click on a heading collapses its section and keeps the size");
        frame();
        check(stats->overallHeight() == expected - 2 * 20.0f, "its two rows are gone");
        // The panel is anchored at the bottom: a shorter panel brings its heading down.
        const float collapsedY = (engineHeader->headerTop + engineHeader->headerBottom) * 0.5f;
        check(collapsedY < headingY, "the heading moved down with the panel's top");
        clickAt(20.0f, collapsedY);
        frame();
        check(!stats->groupCollapsed(MiniStats::ENGINE), "and a second click expands it");

        stats->handleEvent(mouseButton(true, 20.0f, h - 20.0f));
        stats->handleEvent(mouseMotion(40.0f, h - 20.0f));
        stats->handleEvent(mouseButton(false, 40.0f, h - 20.0f));
        check(stats->activeSizeIndex() == 1, "a drag is not a click");

        stats->handleEvent(mouseMotion(20.0f, h - 20.0f));
        check(stats->capturesPointer(), "the pointer over the panel is the panel's");
        stats->handleEvent(mouseMotion(400.0f, h - 20.0f));
        check(!stats->capturesPointer(), "and not once it leaves");
        check(!stats->handleEvent(wheel(20.0f, h - 20.0f, -1.0f)), "a panel that fits does not take the wheel");

        stats->setGroupCollapsed(MiniStats::RESOURCES, false);
        frame();
        int textures = -1;
        for (const auto& g : stats->graphs()) {
            if (g->label == "Textures" && g->countOnly) {
                textures = std::stoi(g->timingText);
            }
        }
        // At least the two fonts' two pages each (the history ring exists only once graphs
        // have been shown).
        check(textures >= 4, "the Textures row counts the device's live textures (" + std::to_string(textures) + ")");

        // Larger size: graphs.
        clickAt(20.0f, 20.0f);
        check(stats->activeSizeIndex() == 2, "the third size");
        for (int i = 0; i < 4; ++i) {
            frame();
        }
        const MiniStatsGraph* drawCalls = nullptr;
        for (const auto& g : stats->graphs()) {
            if (g->name == "DrawCalls") {
                drawCalls = g.get();
            }
        }
        check(drawCalls && drawCalls->quad >= 0, "a graph quad per row");
        int texturesWithGraphs = -1;
        // Counts refresh with the text, every 500 ms; showing the section again counts at once.
        stats->setGroupCollapsed(MiniStats::RESOURCES, true);
        stats->setGroupCollapsed(MiniStats::RESOURCES, false);
        frame();
        for (const auto& g : stats->graphs()) {
            if (g->label == "Textures" && g->countOnly) {
                texturesWithGraphs = std::stoi(g->timingText);
            }
        }
        check(texturesWithGraphs == textures + 3, "showing graphs adds the history ring's three textures");
        check(stats->renderer().material()->params()[0] > 0.0f, "the cursor has moved");

        // Back to compact: within one text window nothing it shows changes, and nothing
        // is rebuilt.
        clickAt(20.0f, 20.0f);
        frame();
        const int base = stats->renderer().meshInstance()->mesh()->getPrimitive().base;
        bool rebuilt = false;
        for (int i = 0; i < 5; ++i) {
            stats->postRender();
            rebuilt |= stats->renderer().meshInstance()->mesh()->getPrimitive().base != base;
        }
        check(!rebuilt, "frames that change no figure commit no geometry");

        Layer* ui = engine->scene()->layers()->getLayerById(LAYERID_UI).get();
        const auto onLayer = [&] {
            const auto& list = ui->meshInstances();
            return std::find(list.begin(), list.end(), stats->renderer().meshInstance()) != list.end();
        };
        check(onLayer(), "on the UI layer");
        stats->setEnabled(false);
        frame();
        check(!onLayer() && appStats.drawCalls().forward == 0, "disabled: off the layer, not drawn");
        check(!stats->handleEvent(mouseButton(true, 20.0f, h - 20.0f)), "and takes no input");
        stats->setEnabled(true);
        frame();
        check(onLayer(), "enabled again: back on");
    }

    std::cout << "\na panel taller than the window\n";
    {
        auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{
            .size = {320, 160}, .cpuBuffers = true, .recordDraws = true});
        auto engine = makeTestEngine<CameraComponentSystem>(device);
        auto regular = makeMsdfTestFont(device.get());
        auto bold = makeMsdfTestFont(device.get());
        MiniStatsOptions options;
        options.regularFont = regular.get();
        options.boldFont = bold.get();
        options.startSizeIndex = 2;
        MiniStats stats(engine, options);
        stats.postRender();
        stats.postRender();
        check(stats.panelHeight() == 160.0f - 16.0f && stats.overallHeight() > stats.panelHeight(),
              "the panel stops 8 points short of each edge");
        check(stats.handleEvent(wheel(20.0f, 160.0f - 20.0f, -1.0f)), "and the wheel scrolls it");
    }

    return finish("mini-stats");
}
