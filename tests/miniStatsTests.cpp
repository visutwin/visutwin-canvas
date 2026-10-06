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

#include <algorithm>
#include <functional>
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
#include "framework/handlers/fontResource.h"
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

    /// The row whose name (a sub-row's stat or pass name) is `name`, or null.
    const MiniStatsGraph* findGraph(const MiniStats& stats, const std::string& name)
    {
        for (const auto& g : stats.graphs()) {
            if (g->name == name) {
                return g.get();
            }
        }
        return nullptr;
    }

    float sample(const MiniStats& stats, const std::string& name)
    {
        const MiniStatsGraph* graph = findGraph(stats, name);
        return graph && graph->sampler ? graph->sampler() : -1.0f;
    }

    /// A panel on a stub engine with its own GPU pass timings, in the size `size`.
    struct PassFixture
    {
        std::shared_ptr<StubGraphicsDevice> device;
        std::shared_ptr<Engine> engine;
        std::shared_ptr<FontResource> regular;
        std::shared_ptr<FontResource> bold;
        std::vector<GpuProfiler::PassTiming> passes;
        std::unique_ptr<MiniStats> stats;

        explicit PassFixture(const int size, const std::function<void(MiniStatsOptions&)>& configure = {})
        {
            device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{
                .size = {640, 480}, .resizable = true, .cpuBuffers = true});
            engine = makeTestEngine<CameraComponentSystem>(device);
            regular = makeMsdfTestFont(device.get());
            bold = makeMsdfTestFont(device.get());
            MiniStatsOptions options;
            options.regularFont = regular.get();
            options.boldFont = bold.get();
            options.startSizeIndex = size;
            options.gpuPassTimings = [this]() { return &passes; };
            if (configure) {
                configure(options);
            }
            stats = std::make_unique<MiniStats>(engine, options);
        }

        FrameStats& frame() { return engine->stats()->frame(); }

        /// Row `row` of the history, width() x 4 bytes.
        std::vector<uint8_t> row(const int row) const
        {
            const MiniStatsHistory& h = stats->history();
            const auto* begin = h.pixels().data() + static_cast<size_t>(row) * h.width() * 4;
            return {begin, begin + static_cast<size_t>(h.width()) * 4};
        }
    };
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

    std::cout << "\nthe CPU row is one frame's update plus render\n";
    {
        // At the hook the update time is this frame's and the render time the previous
        // frame's (the engine writes it after the hook), so the update waits one hook.
        PassFixture f(1);
        FrameStats& frame = f.frame();
        frame.updateTime = 1.0;
        frame.renderTime = 100.0;
        frame.physicsTime = 0.0;
        f.stats->postRender();
        check(sample(*f.stats, "CPU") == 0.0f + 100.0f, "the first hook has no update to pair: 0 + 100");

        frame.updateTime = 2.0;
        frame.renderTime = 10.0;
        f.stats->postRender();
        check(sample(*f.stats, "CPU") == 1.0f + 10.0f, "frame 1's update with frame 1's render");
        check(sample(*f.stats, "updateTime") == 1.0f && sample(*f.stats, "renderTime") == 10.0f,
              "the Update and Render rows show the same frame");

        // A spike in one frame's update and physics is counted with that frame's render only.
        frame.updateTime = 50.0;
        frame.physicsTime = 4.0;
        frame.renderTime = 20.0;
        f.stats->postRender();
        check(sample(*f.stats, "CPU") == 2.0f + 20.0f, "the spike is not yet counted");
        check(!findGraph(*f.stats, "physicsTime"), "nor is its physics row created");

        frame.updateTime = 3.0;
        frame.physicsTime = 0.0;
        frame.renderTime = 30.0;
        f.stats->postRender();
        check(sample(*f.stats, "CPU") == 50.0f + 30.0f, "the spike with its own frame's render");
        check(sample(*f.stats, "physicsTime") == 4.0f, "and the physics row appears with that frame's figure");
        check(sample(*f.stats, "updateTime") + sample(*f.stats, "renderTime") == sample(*f.stats, "CPU"),
              "the rows add up to the CPU row");
    }

    std::cout << "\na GPU pass row appears once its pass reports time\n";
    {
        PassFixture f(1);
        f.passes = {{"Shadow", 0.0}};
        f.stats->postRender();
        check(!findGraph(*f.stats, "Shadow"), "a pass at zero adds no row");
        f.passes = {{"Shadow", 1.5}};
        f.stats->postRender();
        const MiniStatsGraph* shadow = findGraph(*f.stats, "Shadow");
        check(shadow && shadow->group == MiniStats::GPU && shadow->parent == findGraph(*f.stats, "GPU"),
              "a pass above zero: a row under GPU");
        check(sample(*f.stats, "Shadow") == 1.5f, "sampling the pass");
        f.passes = {{"Shadow", 1.5}, {"Forward", 1.0}, {"Forward", 2.0}};
        f.stats->postRender();
        check(sample(*f.stats, "Forward") == 3.0f, "passes sharing a name are one row holding their sum");
    }

    std::cout << "\nGPU pass rows are aged out and their rows reused, cleared\n";
    {
        PassFixture f(2);
        f.passes = {{"Old pass", 12.0}};
        f.stats->postRender();
        const int oldRow = findGraph(*f.stats, "Old pass")->row;
        f.passes.clear();
        for (int i = 0; i < 240; ++i) {
            f.stats->postRender();
        }
        check(findGraph(*f.stats, "Old pass") != nullptr, "240 frames without the pass: still there");
        f.stats->postRender();
        check(!findGraph(*f.stats, "Old pass"), "241: removed");
        bool written = false;
        for (size_t i = 3; i < f.row(oldRow).size(); i += 4) {
            written |= f.row(oldRow)[i] != 0;
        }
        check(written, "the old pass wrote its history into the row");

        f.passes = {{"New pass", 2.0}};
        f.stats->postRender();
        check(findGraph(*f.stats, "New pass")->row == oldRow, "a new pass takes the freed row");
        f.stats->postRender();   // its first sample
        const int column = (f.stats->history().cursor() + f.stats->history().width() - 1) %
                           f.stats->history().width();
        const std::vector<uint8_t> row = f.row(oldRow);
        bool othersClear = true;
        for (size_t i = 0; i < row.size(); ++i) {
            if (i / 4 != static_cast<size_t>(column)) {
                othersClear &= row[i] == 0;
            }
        }
        check(row[static_cast<size_t>(column) * 4 + 3] == 170 && othersClear,
              "the row is cleared before its first sample: nothing of the old pass is left");

        f.stats->setActiveSizeIndex(0);
        bool noOrphans = true;
        for (const auto& g : f.stats->graphs()) {
            noOrphans &= !g->parent || g->parent->headerOnly;
        }
        check(noOrphans && !findGraph(*f.stats, "New pass"), "the compact size drops every sub-row");
    }

    std::cout << "\nnew GPU passes grow the history and keep what it holds\n";
    {
        PassFixture f(2);
        f.stats->postRender();
        f.stats->postRender();
        const int frameRow = findGraph(*f.stats, "Frame")->row;
        // The columns written so far; the growing frame writes one more of its own.
        const auto written = static_cast<size_t>(f.stats->history().cursor()) * 4;
        const std::vector<uint8_t> before = f.row(frameRow);
        check(written == 8 && before[3] == 170 && before[7] == 170, "two samples in the Frame row");
        const int oldHeight = f.stats->history().height();
        for (int i = 0; i < 20; ++i) {
            f.passes.push_back({"Pass." + std::to_string(i), 1.0});
        }
        f.stats->postRender();
        check(f.stats->history().height() > oldHeight, "twenty more rows grow the texture");
        check(std::equal(before.begin(), before.begin() + static_cast<std::ptrdiff_t>(written), f.row(frameRow).begin()),
              "an existing row's samples survive the growth");
        f.stats->postRender();
        check(sample(*f.stats, "Pass.0") == 1.0f, "and a new row samples its pass");
    }

    std::cout << "\nno sub-rows for a category that is off\n";
    {
        PassFixture f(2, [](MiniStatsOptions& o) {
            o.cpuEnabled = false;
            o.gpuEnabled = false;
        });
        f.passes = {{"Pass", 2.0}};
        f.frame().physicsTime = 3.0;
        f.stats->postRender();
        f.stats->postRender();
        int counters = 0;
        bool noCpuGpu = true;
        for (const auto& g : f.stats->graphs()) {
            noCpuGpu &= g->group != MiniStats::CPU && g->group != MiniStats::GPU;
            counters += g->countOnly ? 0 : 1;
        }
        check(noCpuGpu, "no CPU or GPU row of any kind");
        check(counters == 7, "Engine, Draw calls, Frame, VRAM and its three parts (" + std::to_string(counters) + ")");
    }

    std::cout << "\nthe panel follows the canvas without a resize event\n";
    {
        PassFixture f(0);
        f.stats->postRender();
        check(f.stats->panelWidth() == 128.0f && f.stats->panelHeight() == 118.0f, "640x480: the full panel");
        f.device->setResolution(100, 60);
        f.stats->postRender();
        check(f.stats->panelWidth() == 100.0f - 16.0f && f.stats->panelHeight() == 60.0f - 16.0f,
              "100x60: cut to 8 points from each edge");
        f.device->setResolution(640, 480);
        f.stats->postRender();
        check(f.stats->panelWidth() == 128.0f && f.stats->panelHeight() == 118.0f, "and back");
    }

    std::cout << "\na zero-size canvas draws nothing\n";
    {
        auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{
            .size = {640, 480}, .resizable = true, .cpuBuffers = true, .recordDraws = true, .renderTargets = true});
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
        MiniStats stats(engine, options);
        engine->start();
        auto& appStats = *engine->stats();
        // The geometry is built at "postrender", after this frame's draws: a frame draws
        // what the frame before it built.
        const auto draws = [&] {
            engine->update(1.0f / 60.0f);
            engine->render();
            return appStats.drawCalls().forward;
        };
        draws();
        check(draws() == 1, "drawn");
        device->setResolution(0, 0);
        check(draws() == 1, "the frame the canvas collapses still draws the previous panel");
        check(!stats.renderer().meshInstance()->visible() && stats.renderer().quadCount() == 0,
              "but lays out no quads and hides the instance");
        check(draws() == 0, "so the next frame issues no draw");
        device->setResolution(640, 480);
        check(draws() == 0, "restored: the first frame still has nothing to draw");
        check(draws() == 1, "and the next draws the panel again");
    }

    return finish("mini-stats");
}
