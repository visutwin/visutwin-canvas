// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.10.2026
//
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <SDL3/SDL_events.h>

#include "core/eventHandler.h"
#include "platform/graphics/gpuProfiler.h"
#include "miniStatsGraph.h"
#include "miniStatsText.h"
#include "render2d.h"

namespace visutwin::canvas
{
    class Engine;
    class Layer;
    struct FontResource;

    /// One size of the panel. `width` is the panel's, `height` a row's, in points.
    struct MiniStatsSize
    {
        float width = 128.0f;
        float height = 22.0f;
        float spacing = 0.0f;
        bool graphs = false;
        bool detailed = false;
        bool peak = false;
    };

    /// An extra counter row.
    struct MiniStatsGraphOptions
    {
        std::string name;
        MiniStatsGraph::Sampler sample;
        int decimals = 0;
        std::string units;
        float watermark = 100.0f;
        /// Listed in the User section rather than Engine.
        bool user = false;
    };

    struct MiniStatsOptions
    {
        /// Compact counters; grouped averages; grouped averages with peaks and history.
        std::vector<MiniStatsSize> sizes = {
            {128.0f, 22.0f, 0.0f, false, false, false},
            {176.0f, 20.0f, 0.0f, false, true, false},
            {224.0f, 22.0f, 0.0f, true, true, true}};
        int startSizeIndex = 0;
        /// Text update interval and averaging window: each update shows the mean and peak
        /// of the frames since the last one. History samples every frame.
        float textRefreshMs = 500.0f;
        bool resourcesEnabled = true;
        bool resourcesCollapsed = true;
        bool cpuEnabled = true;
        float cpuWatermark = 33.0f;
        bool gpuEnabled = true;
        float gpuWatermark = 33.0f;
        /// Further counters after Draw calls, Frame and VRAM.
        std::vector<MiniStatsGraphOptions> stats;
        /// The size index from which the GPU pass, CPU phase and VRAM part rows show.
        int gpuTimingMinSize = 1;
        int cpuTimingMinSize = 1;
        int vramTimingMinSize = 1;
        /// Where the per-pass GPU timings come from: null for none this frame. Empty, the
        /// device's GPU profiler. Any other source is for a caller with timings of its own
        /// (a test, which has no profiler).
        std::function<const std::vector<GpuProfiler::PassTiming>*()> gpuPassTimings;
        /// MSDF fonts for labels and figures; the HUD draws nothing without both.
        const FontResource* regularFont = nullptr;
        const FontResource* boldFont = nullptr;
    };

    /**
     * A small realtime performance overlay in the bottom-left corner of the window: CPU and
     * GPU time, frame interval, draw calls and VRAM, then per-pass GPU timings, CPU phases,
     * VRAM parts and live resource counts in the detailed sizes.
     *
     * Three sizes by default: compact counters, grouped averages, and grouped averages with
     * peaks and history graphs. A click on the panel goes to the next size; in a detailed
     * size a click on a heading collapses or expands its section instead, and the wheel
     * scrolls a panel taller than the window. Collapsing keeps a section's sampling and
     * history. Figures are the mean over the last textRefreshMs.
     *
     * It is drawn with Render2d on the UI layer, as one draw, by the first camera rendering
     * that layer; someone has to render it (the examples host adds a camera when no camera of
     * the example does). It samples on the engine's "postrender" event and its geometry is
     * rebuilt only when a figure it shows changes. Input reaches it through handleEvent().
     *
     * While shown the HUD keeps the device's GPU profiler enabled (it is off by default because
     * sampling has a small cost). Hiding or destroying the HUD puts the profiler back in the
     * state it was in when the HUD was created or last shown, so a caller that enabled it
     * for its own use keeps it.
     *
     * The CPU row is ONE frame's update plus render. The engine writes the render figure after
     * this hook, so the update figure is held back a frame to pair with it; the Update,
     * Render and Physics rows show the same frame and add up to the CPU row.
     *
     * DEVIATIONS: the input comes from handleEvent() (no keyboard focus, no Enter or Space to
     * change size, no touch drag); the CPU section's rows are Update, Render and Physics,
     * which are the CPU phases this engine measures; the VRAM parts are always all three;
     * the Resources rows count this device's live textures, render targets, vertex, storage
     * and index buffers, shaders, and render and compute pipelines on both backends (no
     * uniform-buffer row: the uniform rings are not separate objects). Frame time is measured at the hook itself, at the
     * performance counter's resolution.
     */
    class MiniStats
    {
    public:
        MiniStats(const std::shared_ptr<Engine>& engine, MiniStatsOptions options = {});
        ~MiniStats();

        MiniStats(const MiniStats&) = delete;
        MiniStats& operator=(const MiniStats&) = delete;

        /// The overlay and its sampling; hidden it costs nothing and samples nothing.
        void setEnabled(bool value);
        [[nodiscard]] bool enabled() const { return _enabled; }

        void setActiveSizeIndex(int value);
        [[nodiscard]] int activeSizeIndex() const { return _activeSizeIndex; }

        void setResourcesEnabled(bool value);
        [[nodiscard]] bool resourcesEnabled() const { return _resourcesEnabled; }

        /// Sections: 1 CPU, 2 GPU, 3 VRAM, 4 User, 5 Engine, 6 Resources.
        enum Group : int { CPU = 1, GPU = 2, VRAM = 3, USER = 4, ENGINE = 5, RESOURCES = 6 };
        void setGroupCollapsed(int group, bool collapsed);
        [[nodiscard]] bool groupCollapsed(int group) const { return _collapsedGroups.contains(group); }

        /**
         * Mouse events in window points. Returns true for an event the panel takes (a press,
         * its release, a wheel step it scrolls with); the caller should not act on those.
         */
        bool handleEvent(const SDL_Event& event);
        /// The pointer is over the panel, or a press that began on it is still held.
        [[nodiscard]] bool capturesPointer() const { return _enabled && (_hovered || _pressed); }

        /// The panel's rectangle in points from the window's bottom-left corner.
        [[nodiscard]] float panelWidth() const { return _panelWidth; }
        [[nodiscard]] float panelHeight() const { return _panelHeight; }
        [[nodiscard]] float overallHeight() const { return _overallHeight; }
        [[nodiscard]] const std::vector<std::unique_ptr<MiniStatsGraph>>& graphs() const { return _graphs; }
        [[nodiscard]] const Render2d& renderer() const { return *_render2d; }
        [[nodiscard]] const MiniStatsHistory& history() const { return *_history; }
        [[nodiscard]] bool textValid() const { return _text.valid(); }

        /// What a frame does on "postrender", for a caller driving it by hand (tests).
        void postRender();

    private:
        void initGraphs();
        MiniStatsGraph* addGraph(std::unique_ptr<MiniStatsGraph> graph);
        void allocateRow(MiniStatsGraph& graph);
        void removeGraph(MiniStatsGraph* graph);
        void clearSubGraphs(std::map<std::string, MiniStatsGraph*>& map);
        void updateSubStat(std::map<std::string, MiniStatsGraph*>& map, MiniStatsGraph* parent,
                           const std::string& name, float value, const std::string& label,
                           const std::string& units, bool delayed, MiniStatsGraph::Sampler sampler);
        [[nodiscard]] bool isGraphVisible(const MiniStatsGraph& graph) const;
        void resize(float width, float height, bool showGraphs);
        void updateLayout();
        void scroll(float delta);
        void update(float ms);
        void render();
        void rebuildGeometry();
        void handleClick(float y);
        [[nodiscard]] bool insidePanel(float x, float y) const;
        [[nodiscard]] const std::vector<GpuProfiler::PassTiming>* passTimings() const;
        void latchCpuTimes();
        [[nodiscard]] GpuProfiler* profiler() const;
        void restoreProfiler();

        std::shared_ptr<Engine> _engine;
        MiniStatsOptions _options;
        std::vector<MiniStatsSize> _sizes;

        std::vector<std::unique_ptr<MiniStatsGraph>> _graphs;   // display order
        std::vector<int> _freeRows;
        int _nextRowIndex = 0;
        std::map<std::string, MiniStatsGraph*> _gpuPassGraphs;
        std::map<std::string, MiniStatsGraph*> _cpuGraphs;
        std::map<std::string, MiniStatsGraph*> _vramGraphs;
        std::map<std::string, MiniStatsGraph*> _resourceGraphs;
        MiniStatsGraph* _cpuGraph = nullptr;
        MiniStatsGraph* _gpuGraph = nullptr;
        MiniStatsGraph* _vramGraph = nullptr;
        MiniStatsGraph* _resourceGraph = nullptr;
        std::set<int> _collapsedGroups;

        bool _resourcesEnabled = true;
        float _resourceElapsed = 0.0f;
        bool _resourcesDue = true;

        std::string _averageLabel;
        int _frameIndex = 0;
        bool _enabled = true;
        bool _showGraphs = false;
        bool _detailed = false;
        bool _showPeak = false;
        bool _geometryDirty = true;
        bool _layoutDirty = true;
        int _activeSizeIndex = 0;
        float _width = 128.0f;
        float _height = 22.0f;
        float _spacing = 0.0f;
        float _scroll = 0.0f;
        float _maxScroll = 0.0f;
        float _overallHeight = 0.0f;
        float _panelWidth = 0.0f;
        float _panelHeight = 0.0f;
        float _canvasWidth = 1.0f;
        float _canvasHeight = 1.0f;
        float _opacity = 0.95f;

        // Frame interval, measured at the hook.
        uint64_t _lastCounter = 0;
        float _frameMs = 0.0f;
        // The CPU figures the rows show, all of one frame: the engine's render time at the
        // hook is the previous frame's, so the update and physics times seen at the previous
        // hook are held (pending) to pair with it.
        float _cpuUpdateMs = 0.0f;
        float _cpuRenderMs = 0.0f;
        float _cpuPhysicsMs = 0.0f;
        float _pendingUpdateMs = 0.0f;
        float _pendingPhysicsMs = 0.0f;
        // The profiler's state before the HUD enabled it.
        bool _profilerWasEnabled = false;
        // This frame's GPU time per pass name, passes sharing a name summed.
        std::map<std::string, float> _passTotals;

        // Pointer state.
        bool _hovered = false;
        bool _pressed = false;
        bool _dragging = false;
        float _pressX = 0.0f;
        float _pressY = 0.0f;

        MiniStatsText _text;
        std::unique_ptr<MiniStatsHistory> _history;
        std::unique_ptr<Render2d> _render2d;
        Layer* _drawLayer = nullptr;
        EventHandlePtr _onPostRender;
    };
}
