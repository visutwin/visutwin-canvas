// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.10.2026
//
#include "miniStats.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#include <SDL3/SDL_timer.h>
#include <spdlog/spdlog.h>

#include "framework/applicationStats.h"
#include "framework/engine.h"
#include "platform/graphics/gpuProfiler.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/scene.h"

namespace visutwin::canvas
{
    namespace
    {
        // Packed 0xAABBGGRR, display space.
        constexpr uint32_t BACKGROUND = 0xff231b15u;
        constexpr uint32_t GROUP_BACKGROUND = 0xff372b22u;
        constexpr uint32_t BORDER = 0xff4e3d30u;
        constexpr uint32_t TEXT = 0xfffaf5f2u;
        constexpr uint32_t MUTED = 0xffc8b8adu;
        // By group.
        constexpr uint32_t kGraphColors[] = {0xff6db1d9u, 0xfff7b884u, 0xffb6d16du, 0xffdda0b8u,
                                             0xff6db1d9u, 0xffc8b8adu, 0xffa4cfb0u};

        constexpr float kMegabyte = 1.0f / (1024.0f * 1024.0f);
        // A GPU pass row not seen for this many frames is removed.
        constexpr int kPassRowLifetime = 240;

        int graphOrder(const MiniStatsGraph& graph)
        {
            if (graph.headerOnly) {
                return 0;
            }
            if (graph.name == "DrawCalls") {
                return 1;
            }
            if (graph.name == "Frame") {
                return 2;
            }
            return 3;
        }

        int groupOrder(const MiniStatsGraph& graph)
        {
            return graph.group == MiniStats::ENGINE ? 0 : graph.group == MiniStats::USER ? 1 : graph.group + 1;
        }

        int nextPowerOfTwo(const int value)
        {
            int result = 1;
            while (result < value) {
                result <<= 1;
            }
            return result;
        }

        using Style = MiniStatsText::Style;
    }

    MiniStats::MiniStats(const std::shared_ptr<Engine>& engine, MiniStatsOptions options)
        : _engine(engine), _options(std::move(options)), _text(_options.regularFont, _options.boldFont)
    {
        _sizes = _options.sizes;
        if (_sizes.empty()) {
            _sizes.push_back(MiniStatsSize{});
        }
        if (_options.resourcesCollapsed) {
            _collapsedGroups.insert(RESOURCES);
        }
        _resourcesEnabled = _options.resourcesEnabled;
        char label[32];
        std::snprintf(label, sizeof(label), "Avg (%gs)", static_cast<double>(_options.textRefreshMs) / 1000.0);
        _averageLabel = label;

        const auto& device = _engine ? _engine->graphicsDevice() : nullptr;
        _history = std::make_unique<MiniStatsHistory>(device.get());
        _render2d = std::make_unique<Render2d>(device);
        _text.applyMsdf(*_render2d);
        if (_engine && _engine->scene() && _engine->scene()->layers()) {
            _drawLayer = _engine->scene()->layers()->getLayerById(LAYERID_UI).get();
        }

        initGraphs();

        // The profiler is off by default because sampling costs a little; the HUD exists to
        // show it. Whatever state it had is what hiding or destroying the HUD puts back.
        if (device && device->gpuProfiler()) {
            _profilerWasEnabled = device->gpuProfiler()->enabled();
            device->gpuProfiler()->setEnabled(true);
        }
        if (_engine) {
            _onPostRender = _engine->on("postrender", [this]() { postRender(); });
        }
        setActiveSizeIndex(std::clamp(_options.startSizeIndex, 0, static_cast<int>(_sizes.size()) - 1));
    }

    MiniStats::~MiniStats()
    {
        if (_onPostRender) {
            _onPostRender->off();
        }
        if (_enabled) {
            restoreProfiler();
        }
        if (_render2d) {
            _render2d->setLayer(nullptr);
        }
    }

    MiniStatsGraph* MiniStats::addGraph(std::unique_ptr<MiniStatsGraph> graph)
    {
        _graphs.push_back(std::move(graph));
        return _graphs.back().get();
    }

    void MiniStats::initGraphs()
    {
        const float refresh = _options.textRefreshMs;
        Engine* engine = _engine.get();
        const auto device = [engine]() -> GraphicsDevice* { return engine ? engine->graphicsDevice().get() : nullptr; };

        if (_options.cpuEnabled) {
            // One frame's update and render, latched by latchCpuTimes().
            _cpuGraph = addGraph(std::make_unique<MiniStatsGraph>("CPU", _options.cpuWatermark, refresh, [this]() {
                return _cpuUpdateMs + _cpuRenderMs;
            }, 1, "ms"));
            _cpuGraph->group = CPU;
        }
        if (_options.gpuEnabled) {
            _gpuGraph = addGraph(std::make_unique<MiniStatsGraph>("GPU", _options.gpuWatermark, refresh, [device]() {
                const GraphicsDevice* d = device();
                return d && d->gpuProfiler() ? static_cast<float>(d->gpuProfiler()->frameMilliseconds()) : 0.0f;
            }, 1, "ms"));
            _gpuGraph->group = GPU;
        }

        std::vector<MiniStatsGraphOptions> stats = {
            {"DrawCalls", [engine]() {
                return engine && engine->stats() ? static_cast<float>(engine->stats()->drawCalls().total) : 0.0f;
            }, 0, "", 1000.0f, false},
            {"Frame", [this]() { return _frameMs; }, 1, "ms", 33.0f, false},
            {"VRAM", [device]() {
                const GraphicsDevice* d = device();
                if (!d) {
                    return 0.0f;
                }
                const auto& v = d->vram();
                return static_cast<float>(static_cast<double>(v.tex) + v.vb + v.ib + v.ub + v.sb) * kMegabyte;
            }, 1, "MB", 1024.0f, false}};
        for (const std::string& preset : _options.statPresets) {
            if (preset == "gsplats") {
                stats.push_back({"GSplats", [engine]() {
                    return engine && engine->stats() ? static_cast<float>(engine->stats()->frame().gsplats) * 1e-6f : 0.0f;
                }, 3, "M", 10.0f, false});
            } else {
                spdlog::warn("MiniStats: no stat preset named '{}'", preset);
            }
        }
        stats.insert(stats.end(), _options.stats.begin(), _options.stats.end());

        MiniStatsGraph* engineHeader = nullptr;
        MiniStatsGraph* userHeader = nullptr;
        for (const auto& entry : stats) {
            auto graph = std::make_unique<MiniStatsGraph>(entry.name, entry.watermark, refresh, entry.sample,
                                                          entry.decimals, entry.units);
            if (entry.name == "VRAM") {
                graph->group = VRAM;
                _vramGraph = graph.get();
            } else {
                MiniStatsGraph*& header = entry.user ? userHeader : engineHeader;
                if (!header) {
                    header = addGraph(std::make_unique<MiniStatsGraph>(entry.user ? "User" : "Engine", 0.0f, refresh,
                                                                       nullptr));
                    header->group = entry.user ? USER : ENGINE;
                    header->headerOnly = true;
                }
                graph->group = header->group;
                graph->parent = header;
            }
            addGraph(std::move(graph));
        }

        _resourceGraph = addGraph(std::make_unique<MiniStatsGraph>("Resources", 0.0f, refresh, nullptr));
        _resourceGraph->group = RESOURCES;
        _resourceGraph->headerOnly = true;
        _resourceGraph->countOnly = true;
        for (const auto& [key, label] : std::vector<std::pair<std::string, std::string>>{
                 {"vertexBuffers", "Vertex buffers"}, {"indexBuffers", "Index buffers"},
                 {"storageBuffers", "Storage buffers"}, {"textures", "Textures"},
                 {"renderTargets", "Render targets"}, {"shaders", "Shaders"},
                 {"renderPipelines", "Render pipelines"}, {"computePipelines", "Compute pipelines"}}) {
            auto* graph = addGraph(std::make_unique<MiniStatsGraph>(label, 0.0f, refresh, nullptr));
            graph->group = RESOURCES;
            graph->parent = _resourceGraph;
            graph->countOnly = true;
            _resourceGraphs[key] = graph;
        }

        std::ranges::stable_sort(_graphs, [](const auto& a, const auto& b) {
            const int ga = groupOrder(*a);
            const int gb = groupOrder(*b);
            return ga != gb ? ga < gb : graphOrder(*a) < graphOrder(*b);
        });
        for (const auto& graph : _graphs) {
            if (!graph->headerOnly) {
                allocateRow(*graph);
            }
        }
    }

    void MiniStats::allocateRow(MiniStatsGraph& graph)
    {
        int row;
        if (!_freeRows.empty()) {
            row = _freeRows.back();
            _freeRows.pop_back();
        } else {
            row = _nextRowIndex++;
        }
        float maxWidth = 1.0f;
        for (const auto& size : _sizes) {
            maxWidth = std::max(maxWidth, size.width);
        }
        const int height = _history->height();
        _history->ensureSize(nextPowerOfTwo(static_cast<int>(std::ceil(maxWidth))), _nextRowIndex);
        if (_history->height() != height) {
            // The graph quads' v is a row's fraction of the height.
            _geometryDirty = true;
        }
        graph.row = row;
        graph.needsClear = true;
    }

    void MiniStats::removeGraph(MiniStatsGraph* graph)
    {
        const auto it = std::ranges::find_if(_graphs, [graph](const auto& g) { return g.get() == graph; });
        if (it == _graphs.end()) {
            return;
        }
        if (graph->row >= 0) {
            _freeRows.push_back(graph->row);
        }
        _graphs.erase(it);
        _layoutDirty = true;
    }

    void MiniStats::clearSubGraphs(std::map<std::string, MiniStatsGraph*>& map)
    {
        for (const auto& [name, graph] : map) {
            removeGraph(graph);
        }
        map.clear();
    }

    void MiniStats::setActiveSizeIndex(const int value)
    {
        if (value < 0 || value >= static_cast<int>(_sizes.size())) {
            return;
        }
        const MiniStatsSize& size = _sizes[static_cast<size_t>(value)];
        _activeSizeIndex = value;
        _resourcesDue = true;
        _detailed = size.detailed;
        _showPeak = _detailed && size.peak;
        _spacing = size.spacing;
        _scroll = 0.0f;
        if (!_detailed || value < _options.gpuTimingMinSize) {
            clearSubGraphs(_gpuPassGraphs);
        }
        if (!_detailed || value < _options.cpuTimingMinSize) {
            clearSubGraphs(_cpuGraphs);
        }
        if (!_detailed || value < _options.vramTimingMinSize) {
            clearSubGraphs(_vramGraphs);
        }
        resize(size.width, size.height, size.graphs);
    }

    void MiniStats::setEnabled(const bool value)
    {
        if (value == _enabled) {
            return;
        }
        _enabled = value;
        _resourcesDue = true;
        for (const auto& graph : _graphs) {
            graph->enabled = value && _showGraphs;
        }
        if (!value) {
            _hovered = false;
            _pressed = false;
            _render2d->setLayer(nullptr);
        }
        // Shown, the HUD needs the profiler; hidden, it leaves it as it found it.
        if (value) {
            if (GpuProfiler* profiler = this->profiler()) {
                _profilerWasEnabled = profiler->enabled();
                profiler->setEnabled(true);
            }
        } else {
            restoreProfiler();
        }
        _lastCounter = 0;
    }

    void MiniStats::setResourcesEnabled(const bool value)
    {
        if (value != _resourcesEnabled) {
            _resourcesEnabled = value;
            _resourcesDue = true;
            _layoutDirty = true;
        }
    }

    void MiniStats::setGroupCollapsed(const int group, const bool collapsed)
    {
        if (_collapsedGroups.contains(group) == collapsed) {
            return;
        }
        if (group == RESOURCES) {
            _resourcesDue = true;
        }
        if (collapsed) {
            _collapsedGroups.insert(group);
        } else {
            _collapsedGroups.erase(group);
        }
        _layoutDirty = true;
    }

    bool MiniStats::isGraphVisible(const MiniStatsGraph& graph) const
    {
        if (graph.countOnly && (!_resourcesEnabled || !_detailed)) {
            return false;
        }
        if (!_detailed) {
            return !graph.headerOnly;
        }
        return !graph.parent || !_collapsedGroups.contains(graph.group);
    }

    void MiniStats::resize(const float width, const float height, const bool showGraphs)
    {
        _width = width;
        _height = height;
        _showGraphs = showGraphs;
        for (const auto& graph : _graphs) {
            graph->enabled = _enabled && showGraphs;
        }
        _layoutDirty = true;
    }

    void MiniStats::updateLayout()
    {
        if (_engine) {
            const auto [w, h] = _engine->canvasSize();
            _canvasWidth = static_cast<float>(std::max(w, 1));
            _canvasHeight = static_cast<float>(std::max(h, 1));
        }
        _render2d->setTargetSize(_canvasWidth, _canvasHeight);
        float total = _detailed ? 31.0f : 8.0f;
        int previousGroup = -1;
        int visibleRows = 0;
        for (const auto& graph : _graphs) {
            if (!isGraphVisible(*graph)) {
                continue;
            }
            if (_detailed && visibleRows > 0 && graph->group != previousGroup) {
                total += 5.0f;
            }
            total += _height + (visibleRows ? _spacing : 0.0f);
            visibleRows++;
            previousGroup = graph->group;
        }
        _overallHeight = total;
        _panelWidth = std::max(0.0f, std::min(_width, _canvasWidth - 16.0f));
        _panelHeight = std::max(0.0f, std::min(total, _canvasHeight - 16.0f));
        _maxScroll = std::max(0.0f, total - _panelHeight);
        _scroll = std::min(_scroll, _maxScroll);
        _layoutDirty = false;
        _geometryDirty = true;
    }

    void MiniStats::scroll(const float delta)
    {
        const float value = std::clamp(_scroll + delta, 0.0f, _maxScroll);
        if (value != _scroll) {
            _scroll = value;
            _geometryDirty = true;
        }
    }

    void MiniStats::update(const float ms, const bool sample)
    {
        if (_resourcesEnabled && _detailed) {
            _resourceElapsed += ms;
            if (_resourcesDue || _resourceElapsed >= _options.textRefreshMs) {
                _resourcesDue = false;
                _resourceElapsed = 0.0f;
                GraphicsDevice::LiveResourceCounts counts;
                if (_engine && _engine->graphicsDevice()) {
                    counts = _engine->graphicsDevice()->liveResourceCounts();
                }
                const std::pair<const char*, int> values[] = {
                    {"vertexBuffers", counts.vertexBuffers}, {"indexBuffers", counts.indexBuffers},
                    {"storageBuffers", counts.storageBuffers}, {"textures", counts.textures},
                    {"renderTargets", counts.renderTargets}, {"shaders", counts.shaders},
                    {"renderPipelines", counts.renderPipelines}, {"computePipelines", counts.computePipelines}};
                int total = 0;
                for (const auto& [key, count] : values) {
                    MiniStatsGraph* graph = _resourceGraphs[key];
                    graph->count = count;
                    total += count;
                    const std::string text = std::to_string(count);
                    if (graph->timingText != text) {
                        graph->timingText = text;
                        _geometryDirty = true;
                    }
                }
                const std::string totalText = std::to_string(total);
                if (_resourceGraph->timingText != totalText) {
                    _resourceGraph->timingText = totalText;
                    _geometryDirty = true;
                }
            }
        }
        if (!sample) {
            return;
        }
        MiniStatsHistory* history = _showGraphs ? _history.get() : nullptr;
        for (const auto& graph : _graphs) {
            if (graph->countOnly && (!_resourcesEnabled || !_detailed)) {
                continue;
            }
            const int changed = graph->update(ms, history);
            if (changed & (_showPeak ? 3 : 1)) {
                _geometryDirty = true;
            }
        }
        if (history) {
            history->advance();
        }
    }

    void MiniStats::updateSubStat(std::map<std::string, MiniStatsGraph*>& map, MiniStatsGraph* parent,
                                  const std::string& name, const float value, const std::string& label,
                                  const std::string& units, const bool delayed, MiniStatsGraph::Sampler sampler)
    {
        if (!parent) {
            return;
        }
        MiniStatsGraph* graph = nullptr;
        if (const auto it = map.find(name); it != map.end()) {
            graph = it->second;
        } else {
            if (delayed && !(value > 0.0f)) {
                return;
            }
            auto created = std::make_unique<MiniStatsGraph>(name, parent->watermark, _options.textRefreshMs,
                                                            std::move(sampler), 1, units);
            created->label = label;
            created->parent = parent;
            created->group = parent->group;
            created->statName = name;
            created->enabled = _enabled && _showGraphs;
            allocateRow(*created);
            // After the parent's existing sub-rows.
            auto at = std::ranges::find_if(_graphs, [parent](const auto& g) { return g.get() == parent; });
            if (at != _graphs.end()) {
                ++at;
            }
            while (at != _graphs.end() && (*at)->parent == parent) {
                ++at;
            }
            graph = created.get();
            _graphs.insert(at, std::move(created));
            map[name] = graph;
            _layoutDirty = true;
        }
        graph->watermark = parent->watermark;
        if (value > 0.0f) {
            graph->lastNonZeroFrame = _frameIndex;
        }
    }

    void MiniStats::postRender()
    {
        if (!_enabled) {
            return;
        }

        // The frame interval at this hook: it runs once per rendered frame whichever loop
        // drives the engine, and the performance counter is finer than the stats' ticks.
        const uint64_t counter = SDL_GetPerformanceCounter();
        const uint64_t frequency = SDL_GetPerformanceFrequency();
        // The first frame after the HUD is created or shown has no previous hook to measure
        // from, and its CPU figures pair with a frame the HUD never saw: it refreshes the
        // panel and the resource counts, and samples nothing.
        const bool sample = _lastCounter != 0;
        _frameMs = (sample && frequency != 0)
            ? static_cast<float>(static_cast<double>(counter - _lastCounter) * 1000.0 / static_cast<double>(frequency))
            : 0.0f;
        _lastCounter = counter;

        latchCpuTimes();

        // Passes sharing a name are one row holding their sum: the forward pass draws the
        // scene and then the UI layer, and a separable blur runs twice.
        _passTotals.clear();
        const GraphicsDevice* device = _engine ? _engine->graphicsDevice().get() : nullptr;
        if (const auto* timings = passTimings()) {
            for (const auto& timing : *timings) {
                _passTotals[timing.name] += static_cast<float>(timing.milliseconds);
            }
        }

        update(_frameMs, sample);
        _frameIndex++;

        if (_detailed && _engine) {
            if (_gpuGraph && _activeSizeIndex >= _options.gpuTimingMinSize) {
                for (const auto& [name, ms] : _passTotals) {
                    updateSubStat(_gpuPassGraphs, _gpuGraph, name, ms, name, "ms", true, [this, name]() {
                        const auto it = _passTotals.find(name);
                        return it != _passTotals.end() ? it->second : 0.0f;
                    });
                }
                for (auto it = _gpuPassGraphs.begin(); it != _gpuPassGraphs.end();) {
                    if (_frameIndex - it->second->lastNonZeroFrame > kPassRowLifetime) {
                        removeGraph(it->second);
                        it = _gpuPassGraphs.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            if (_cpuGraph && _activeSizeIndex >= _options.cpuTimingMinSize) {
                // The latched figures, so the rows show the CPU row's frame and add up to it.
                const auto latched = [this](const float MiniStats::* field) {
                    return [this, field]() { return this->*field; };
                };
                updateSubStat(_cpuGraphs, _cpuGraph, "renderTime", _cpuRenderMs, "Render", "ms", false,
                              latched(&MiniStats::_cpuRenderMs));
                updateSubStat(_cpuGraphs, _cpuGraph, "updateTime", _cpuUpdateMs, "Update", "ms", false,
                              latched(&MiniStats::_cpuUpdateMs));
                updateSubStat(_cpuGraphs, _cpuGraph, "physicsTime", _cpuPhysicsMs, "Physics", "ms", true,
                              latched(&MiniStats::_cpuPhysicsMs));
            }
            if (_vramGraph && _activeSizeIndex >= _options.vramTimingMinSize && device) {
                const auto& v = device->vram();
                Engine* engine = _engine.get();
                const auto part = [engine](int (*bytes)(const DeviceVRAM&)) {
                    return [engine, bytes]() {
                        return static_cast<float>(bytes(engine->graphicsDevice()->vram())) * kMegabyte;
                    };
                };
                updateSubStat(_vramGraphs, _vramGraph, "tex", static_cast<float>(v.tex), "Textures", "MB", false,
                              part([](const DeviceVRAM& m) { return m.tex; }));
                updateSubStat(_vramGraphs, _vramGraph, "geom", static_cast<float>(v.vb + v.ib), "Geometry", "MB",
                              false, part([](const DeviceVRAM& m) { return m.vb + m.ib; }));
                updateSubStat(_vramGraphs, _vramGraph, "buffers", static_cast<float>(v.ub + v.sb), "Buffers", "MB",
                              false, part([](const DeviceVRAM& m) { return m.ub + m.sb; }));
            }
        }
        render();
    }

    GpuProfiler* MiniStats::profiler() const
    {
        return _engine && _engine->graphicsDevice() ? _engine->graphicsDevice()->gpuProfiler().get() : nullptr;
    }

    void MiniStats::restoreProfiler()
    {
        if (GpuProfiler* profiler = this->profiler()) {
            profiler->setEnabled(_profilerWasEnabled);
        }
    }

    void MiniStats::latchCpuTimes()
    {
        // At this hook the engine's update and physics figures are this frame's, its render
        // figure the previous frame's (Engine::render writes it after "postrender"). Summed as
        // they stand, a spike in one frame's update would pair with the frame before's render
        // and the peak would show a frame that never happened, so the update and physics
        // figures wait one hook for their render.
        const FrameStats* frame = _engine && _engine->stats() ? &_engine->stats()->frame() : nullptr;
        _cpuUpdateMs = _pendingUpdateMs;
        _cpuPhysicsMs = _pendingPhysicsMs;
        _cpuRenderMs = frame ? static_cast<float>(frame->renderTime) : 0.0f;
        _pendingUpdateMs = frame ? static_cast<float>(frame->updateTime) : 0.0f;
        _pendingPhysicsMs = frame ? static_cast<float>(frame->physicsTime) : 0.0f;
    }

    const std::vector<GpuProfiler::PassTiming>* MiniStats::passTimings() const
    {
        if (_options.gpuPassTimings) {
            return _options.gpuPassTimings();
        }
        const GraphicsDevice* device = _engine ? _engine->graphicsDevice().get() : nullptr;
        return device && device->gpuProfiler() ? &device->gpuProfiler()->passTimings() : nullptr;
    }

    void MiniStats::render()
    {
        if (_engine) {
            const auto [w, h] = _engine->canvasSize();
            if (static_cast<float>(w) != _canvasWidth || static_cast<float>(h) != _canvasHeight) {
                _layoutDirty = true;
            }
        }
        if (_layoutDirty) {
            updateLayout();
        }
        if (_geometryDirty) {
            _geometryDirty = false;
            rebuildGeometry();
        }
        Texture* history = _showGraphs ? _history->upload() : _history->current();
        _render2d->setTextures(_text.textures(history));
        _render2d->setGraphCursor(static_cast<float>(_history->cursor()), static_cast<float>(_history->width()));
        _render2d->setColor(1.0f, 1.0f, 1.0f, _opacity);
        _render2d->render(_text.valid() ? _drawLayer : nullptr);
    }

    void MiniStats::rebuildGeometry()
    {
        Render2d& renderer = *_render2d;
        const MiniStatsText& text = _text;
        const float x = 8.0f;
        const float bottom = 8.0f;
        const float width = _panelWidth;
        const float top = bottom + _panelHeight;
        const float right = x + width - 10.0f;
        const float avgRight = right - (_showPeak ? 44.0f : 0.0f);
        const auto historyWidth = static_cast<float>(_history->width());
        const auto historyHeight = static_cast<float>(_history->height());
        const bool resourcesCollapsed = _collapsedGroups.contains(RESOURCES);

        renderer.startFrame();
        renderer.setClip(x, bottom, width, _panelHeight);
        renderer.rect(x, bottom, width, _panelHeight, BACKGROUND);
        float rowTop = top - 4.0f;
        if (_detailed) {
            const float baseline = top - 16.0f;
            text.render(renderer, "Metric", x + 10.0f, baseline, Style::Regular, MUTED);
            text.render(renderer, _averageLabel, avgRight - text.measure(_averageLabel, Style::Regular), baseline,
                        Style::Regular, MUTED);
            if (_showPeak) {
                text.render(renderer, "Peak", right - text.measure("Peak", Style::Regular), baseline, Style::Regular,
                            MUTED);
            }
            renderer.rect(x, top - 23.0f, width, 1.0f, BORDER);
            rowTop = top - 27.0f;
        }
        int previousGroup = -1;
        const float clipTop = rowTop;
        renderer.setClip(x, bottom, width, std::max(0.0f, rowTop - bottom));
        rowTop += _scroll;
        for (size_t i = 0; i < _graphs.size(); ++i) {
            MiniStatsGraph& graph = *_graphs[i];
            graph.quad = -1;
            graph.headerTop = 0.0f;
            graph.headerBottom = 0.0f;
            if (!isGraphVisible(graph)) {
                continue;
            }
            if (_detailed && i > 0 && graph.group != previousGroup) {
                rowTop -= 5.0f;
            }
            const float y = rowTop - _height;
            if (y < top && rowTop > bottom) {
                const bool heading = _detailed && graph.group > 0 && !graph.parent;
                const uint32_t color = kGraphColors[std::clamp(graph.group, 0, 6)];
                if (heading) {
                    // A scrolled heading must not take clicks through the column labels.
                    graph.headerTop = std::min(rowTop, clipTop);
                    graph.headerBottom = std::max(y, bottom);
                    renderer.rect(x, y, width, _height, GROUP_BACKGROUND);
                    renderer.rect(x, y + 5.0f, 2.0f, _height - 10.0f, color);
                }
                if (_showGraphs && !graph.headerOnly && graph.row >= 0) {
                    graph.quad = renderer.graph(x, y, width, _height, graph.row, historyWidth, historyHeight, color);
                }
                const float baseline = std::round(y + (_height - 14.0f) / 2.0f + 3.0f);
                const std::string& units = graph.units;
                const bool showUnits = _detailed && !units.empty() && (!graph.parent || graph.parent->headerOnly);
                const bool hasValue = !graph.headerOnly || (graph.countOnly && resourcesCollapsed);
                const float valueWidth = hasValue ? text.measure(graph.timingText, Style::Bold) : 0.0f;
                float valueRight = graph.countOnly ? right : avgRight;
                if (heading && graph.countOnly && !resourcesCollapsed) {
                    text.render(renderer, "Count", right - text.measure("Count", Style::Regular), baseline,
                                Style::Regular, MUTED);
                }
                if (!_detailed && !units.empty()) {
                    const float unitsWidth = text.measure(units, Style::Regular);
                    text.render(renderer, units, right - unitsWidth, baseline, Style::Regular, MUTED);
                    valueRight -= unitsWidth + 4.0f;
                }
                const float valueX = hasValue ? std::max(x + 10.0f, valueRight - valueWidth) : right;
                if (hasValue) {
                    text.render(renderer, graph.timingText, valueX, baseline, Style::Bold, TEXT, valueRight - valueX);
                }
                if (_showPeak && !graph.headerOnly && !graph.countOnly) {
                    const float peakWidth = text.measure(graph.maxText, Style::Regular);
                    text.render(renderer, graph.maxText, right - std::min(peakWidth, 40.0f), baseline, Style::Regular,
                                MUTED, 40.0f);
                }
                const float labelX = x + 10.0f + (_detailed && graph.parent ? 5.0f : 0.0f);
                const float unitsReserve = showUnits ? text.measure(units, Style::Regular) + 4.0f : 0.0f;
                const float labelWidth = text.render(renderer, graph.label, labelX, baseline,
                                                     heading ? Style::Bold : Style::Regular, heading ? TEXT : MUTED,
                                                     valueX - labelX - 8.0f - unitsReserve);
                if (showUnits) {
                    text.render(renderer, units, labelX + labelWidth + 4.0f, baseline, Style::Regular, MUTED,
                                valueX - labelX - labelWidth - 8.0f);
                }
            }
            rowTop = y - _spacing;
            previousGroup = graph.group;
        }
    }

    bool MiniStats::insidePanel(const float x, const float y) const
    {
        return x >= 8.0f && x < 8.0f + _panelWidth && y >= 8.0f && y < 8.0f + _panelHeight;
    }

    void MiniStats::handleClick(const float y)
    {
        if (_detailed) {
            for (const auto& graph : _graphs) {
                if (y >= graph->headerBottom && y < graph->headerTop) {
                    setGroupCollapsed(graph->group, !_collapsedGroups.contains(graph->group));
                    return;
                }
            }
        }
        setActiveSizeIndex((_activeSizeIndex + 1) % static_cast<int>(_sizes.size()));
    }

    bool MiniStats::handleEvent(const SDL_Event& event)
    {
        if (!_enabled) {
            return false;
        }
        // Window points, y measured up from the bottom as the panel is laid out.
        switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION: {
            const float x = event.motion.x;
            const float y = _canvasHeight - event.motion.y;
            _hovered = insidePanel(x, y);
            _opacity = _hovered ? 1.0f : 0.95f;
            if (_pressed && (std::abs(x - _pressX) > 5.0f || std::abs(y - _pressY) > 5.0f)) {
                _dragging = true;
            }
            return _pressed;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            const float x = event.button.x;
            const float y = _canvasHeight - event.button.y;
            if (event.button.button != SDL_BUTTON_LEFT || !insidePanel(x, y)) {
                return false;
            }
            _pressed = true;
            _dragging = false;
            _pressX = x;
            _pressY = y;
            return true;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (event.button.button != SDL_BUTTON_LEFT || !_pressed) {
                return false;
            }
            _pressed = false;
            const float x = event.button.x;
            const float y = _canvasHeight - event.button.y;
            if (!_dragging && insidePanel(x, y)) {
                handleClick(y);
            }
            _dragging = false;
            return true;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            const float x = event.wheel.mouse_x;
            const float y = _canvasHeight - event.wheel.mouse_y;
            if (_maxScroll <= 0.0f || !insidePanel(x, y)) {
                return false;
            }
            // A notch is 100 pixels down the page, as a browser reports it; the system's
            // natural-scrolling setting is already in the sign.
            scroll(-event.wheel.y * 100.0f);
            return true;
        }
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            _hovered = false;
            _pressed = false;
            _dragging = false;
            _opacity = 0.95f;
            return false;
        default:
            return false;
        }
    }
}
