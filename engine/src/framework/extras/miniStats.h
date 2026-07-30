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
     * A small realtime performance overlay (upstream extras/mini-stats). Shows CPU frame time,
     * GPU frame time, draw calls and primitive counts as rolling graphs, plus the per-pass GPU
     * timings resolved by GraphicsDevice::gpuProfiler().
     *
     * Construct it once after the ImGui overlay is initialised; it hooks the engine's "postrender"
     * event and draws itself. That hook is the only safe place for this: ImGuiOverlay::beginFrame()
     * and renderToGPU() both need the frame's drawable, which frameEnd() presents and clears — so
     * driving ImGui after Engine::render() returns would silently draw nothing.
     *
     * Enabling the HUD also enables the GPU profiler, which is off by default because sampling has
     * a small cost.
     *
     * DEVIATION: upstream renders its own graphs through a WordAtlas/Render2d pair on the UI layer
     * and cycles through a configurable `sizes` array on click. This draws an ImGui window
     * instead, with a compact/expanded toggle rather than arbitrary size steps.
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

        /// Compact shows just the two frame-time graphs; expanded adds the counters, the CPU
        /// breakdown and the per-pass GPU timings.
        void setExpanded(const bool value) { _expanded = value; }
        bool expanded() const { return _expanded; }
        void toggleExpanded() { _expanded = !_expanded; }

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

        void plot(const char* label, const History& history, const char* unit) const;

        std::shared_ptr<Engine> _engine;
        ImGuiOverlay* _overlay = nullptr;

        bool _enabled = true;
        bool _expanded = true;

        float _fps = 0.0f;
        uint64_t _lastCounter = 0;

        History _cpuFrame;
        History _gpuFrame;
        History _drawCalls;

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
