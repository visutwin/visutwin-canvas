// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/eventHandler.h"

namespace visutwin::canvas
{
    class Engine;
    class ImGuiOverlay;

    /**
     * A small realtime performance overlay (upstream extras/mini-stats).
     *
     * The default COMPACT view is upstream's first size, row for row: draw calls, frame time,
     * CPU time, GPU time and VRAM, no graphs, anchored to the bottom-left corner of the window,
     * each figure the mean over the last half second (upstream's `textRefreshRate`). Clicking
     * the panel switches to the DETAILED view, which adds the frame-rate line, the rolling
     * graphs, the draw-call and texture breakdowns and the per-pass GPU timings resolved by
     * GraphicsDevice::gpuProfiler(); clicking again returns to compact.
     *
     * Construct it once after the ImGui overlay is initialised; it hooks the engine's "postrender"
     * event and draws itself. That hook is the only safe place for this: ImGuiOverlay::beginFrame()
     * and renderToGPU() both need the frame's drawable, which frameEnd() presents and clears — so
     * driving ImGui after Engine::render() returns would silently draw nothing.
     *
     * Enabling the HUD also enables the GPU profiler, which is off by default because sampling has
     * a small cost.
     *
     * DEVIATION: upstream renders its own rows through a WordAtlas/Render2d pair on the UI layer
     * and cycles THREE sizes on click — compact counters, grouped averages, grouped averages with
     * peaks and graph history. This draws an ImGui window with two views: compact matches
     * upstream's first size, detailed folds its other two into one.
     */
    class MiniStats
    {
    public:
        MiniStats(const std::shared_ptr<Engine>& engine, ImGuiOverlay* overlay);
        ~MiniStats();

        MiniStats(const MiniStats&) = delete;
        MiniStats& operator=(const MiniStats&) = delete;

        void setEnabled(bool value);
        bool enabled() const { return _enabled; }

        /// Compact is the five-row default; detailed adds the graphs, the breakdowns and the
        /// per-pass GPU timings. A click on the panel toggles it, as a click cycles upstream's
        /// sizes.
        void setDetailed(const bool value) { _detailed = value; }
        bool detailed() const { return _detailed; }
        void toggleDetailed() { _detailed = !_detailed; }

    private:
        void draw();

        // Rolling history for the graphs. ImGui::PlotLines reads a flat float array, so samples
        // are kept in insertion order with an explicit offset rather than a wrapping iterator.
        static constexpr int kHistory = 120;

        struct History
        {
            std::array<float, kHistory> samples{};
            int offset = 0;
            float latest = 0.0f;

            void push(float value)
            {
                samples[offset] = value;
                offset = (offset + 1) % kHistory;
                latest = value;
            }

            float maximum() const
            {
                float result = 0.0f;
                for (const float sample : samples) {
                    result = sample > result ? sample : result;
                }
                return result;
            }

            float average() const
            {
                float total = 0.0f;
                for (const float sample : samples) {
                    total += sample;
                }
                return total / static_cast<float>(kHistory);
            }
        };

        // Upstream's Graph keeps two things per stat: the per-frame history its graph draws, and
        // a text figure rewritten every `textRefreshRate` milliseconds as the MEAN of the frames
        // since the last rewrite. The text is what the compact rows show, and the refresh is what
        // keeps them readable — a figure rewritten at 60 Hz is a blur.
        static constexpr float kTextRefreshMs = 500.0f;

        struct Counter
        {
            History history;
            float displayed = 0.0f;

            float accumTotal = 0.0f;
            float accumMs = 0.0f;
            int accumCount = 0;

            void push(const float value, const float frameMs)
            {
                history.push(value);
                accumTotal += value;
                accumMs += frameMs;
                accumCount++;
                if (accumMs >= kTextRefreshMs) {
                    displayed = accumTotal / static_cast<float>(accumCount);
                    accumTotal = 0.0f;
                    accumMs = 0.0f;
                    accumCount = 0;
                }
            }
        };

        void compactRow(const char* label, float value, int decimals, const char* units) const;
        void plot(const char* label, const History& history, const char* unit) const;

        std::shared_ptr<Engine> _engine;
        ImGuiOverlay* _overlay = nullptr;

        bool _enabled = true;
        bool _detailed = false;

        float _fps = 0.0f;
        uint64_t _lastCounter = 0;

        Counter _frame;
        Counter _cpu;
        Counter _gpu;
        Counter _drawCalls;
        Counter _vram;

        // Per-pass GPU timings, keyed by pass name so a graph survives frames where a pass is
        // absent (the profiler resolves results a couple of frames late).
        struct PassHistory
        {
            std::string name;
            History history;
        };
        std::vector<PassHistory> _passHistories;

        void recordPassTimings();

        EventHandlePtr _onPostRender;
    };
}
