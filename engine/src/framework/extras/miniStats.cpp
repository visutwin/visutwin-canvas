// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#include "miniStats.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "imgui.h"

#include "framework/applicationStats.h"
#include "framework/engine.h"
#include "platform/graphics/graphicsDevice.h"
#include "viz/overlay/imguiOverlay.h"

namespace visutwin::canvas
{
    namespace
    {
        // Upstream's panel geometry: 8 px in from the left and bottom edges, and 128 px wide in
        // its compact size. The detailed view sizes itself to its content.
        constexpr float kInset = 8.0f;
        constexpr float kCompactWidth = 128.0f;
    }

    MiniStats::MiniStats(const std::shared_ptr<Engine>& engine, ImGuiOverlay* overlay)
        : _engine(engine), _overlay(overlay)
    {
        // The profiler is off by default because stage-boundary sampling costs a little; the HUD
        // exists to show it, so turn it on.
        if (_engine && _engine->graphicsDevice()) {
            if (const auto& profiler = _engine->graphicsDevice()->gpuProfiler()) {
                profiler->setEnabled(true);
            }
        }

        if (_engine) {
            _onPostRender = _engine->on("postrender", [this]() {
                draw();
            });
        }
    }

    MiniStats::~MiniStats()
    {
        if (_onPostRender) {
            _onPostRender->off();
        }
    }

    void MiniStats::setEnabled(const bool value)
    {
        _enabled = value;

        // Stop paying for GPU sampling while hidden.
        if (_engine && _engine->graphicsDevice()) {
            if (const auto& profiler = _engine->graphicsDevice()->gpuProfiler()) {
                profiler->setEnabled(value);
            }
        }
    }

    void MiniStats::recordPassTimings()
    {
        const auto& device = _engine->graphicsDevice();
        const auto& profiler = device ? device->gpuProfiler() : nullptr;
        if (!profiler) {
            return;
        }

        ++_passFrame;
        // Passes sharing a name are ONE row holding their sum, as upstream's
        // gpu-profiler accumulates them: the forward pass draws the scene and then
        // the UI layer, and a separable blur runs twice. Keyed on the last of them
        // the row showed the 0.1 ms UI pass and hid the 3 ms scene pass behind it.
        std::vector<std::pair<std::string, float>> frameTotals;
        for (const auto& timing : profiler->passTimings()) {
            const auto same = std::ranges::find_if(frameTotals,
                [&timing](const auto& entry) { return entry.first == timing.name; });
            if (same != frameTotals.end()) {
                same->second += static_cast<float>(timing.milliseconds);
            } else {
                frameTotals.emplace_back(timing.name, static_cast<float>(timing.milliseconds));
            }
        }
        for (const auto& [name, milliseconds] : frameTotals) {
            const auto existing = std::ranges::find_if(_passHistories,
                [&name](const PassHistory& entry) { return entry.name == name; });

            if (existing != _passHistories.end()) {
                existing->history.push(milliseconds);
                existing->lastSeenFrame = _passFrame;
            } else {
                PassHistory entry;
                entry.name = name;
                entry.history.push(milliseconds);
                entry.lastSeenFrame = _passFrame;
                _passHistories.push_back(std::move(entry));
            }
        }
        std::erase_if(_passHistories, [this](const PassHistory& entry) {
            return _passFrame - entry.lastSeenFrame > kPassRowLifetime;
        });
    }

    void MiniStats::compactRow(const char* label, const float value, const int decimals,
        const char* units) const
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%.*f", decimals, value);

        // Upstream's compact row: the label muted at the left, the value right-aligned, and the
        // units muted after it. SameLine() takes an offset from the window's left edge, which is
        // the frame GetCursorPosX() reports in, so the two agree without a padding term.
        const float left = ImGui::GetCursorPosX();
        const float right = left + ImGui::GetContentRegionAvail().x;
        const float unitsWidth = *units ? ImGui::CalcTextSize(units).x : 0.0f;
        const float valueRight = *units
            ? right - unitsWidth - ImGui::GetStyle().ItemInnerSpacing.x
            : right;

        ImGui::TextDisabled("%s", label);
        ImGui::SameLine(valueRight - ImGui::CalcTextSize(text).x);
        ImGui::TextUnformatted(text);
        if (*units) {
            ImGui::SameLine(right - unitsWidth);
            ImGui::TextDisabled("%s", units);
        }
    }

    void MiniStats::plot(const char* label, const History& history, const char* unit) const
    {
        // Scale to the window's own peak rather than a fixed range, so both a 1 ms and a 30 ms
        // frame are readable. The floor keeps an idle graph from filling with noise.
        const float peak = std::max(history.maximum() * 1.15f, 0.001f);

        char overlayText[64];
        std::snprintf(overlayText, sizeof(overlayText), "%.2f %s (avg %.2f)",
            history.latest, unit, history.average());

        ImGui::PlotLines(label, history.samples.data(), kHistory, history.offset,
            overlayText, 0.0f, peak, ImVec2(0.0f, 40.0f));
    }

    void MiniStats::draw()
    {
        if (!_enabled || !_overlay || !_engine || !_overlay->isInitialized()) {
            return;
        }

        const auto& stats = _engine->stats();
        const auto& device = _engine->graphicsDevice();
        if (!stats || !device) {
            return;
        }

        // Frame time is measured here rather than read from ApplicationStats: this hook runs
        // exactly once per rendered frame, it keeps the HUD working whichever loop drives the
        // engine, and the counter is high-resolution where the stats path's SDL_GetTicks is
        // whole milliseconds (a 16/17 flicker at 60 Hz).
        const uint64_t counter = SDL_GetPerformanceCounter();
        const uint64_t frequency = SDL_GetPerformanceFrequency();
        float frameMs = 0.0f;
        if (_lastCounter != 0 && frequency != 0) {
            frameMs = static_cast<float>(
                static_cast<double>(counter - _lastCounter) * 1000.0 / static_cast<double>(frequency));
        }
        _lastCounter = counter;

        const auto& frame = stats->frame();
        const auto& drawCalls = stats->drawCalls();
        const auto& profiler = device->gpuProfiler();
        const auto& vram = device->vram();
        constexpr double toMb = 1.0 / (1024.0 * 1024.0);

        // CPU is upstream's CpuTimer: the update phase plus the render phase, on the CPU. The
        // render figure is the previous frame's (Engine::render writes it after frameEnd, this
        // hook runs before), which is one frame of lag upstream carries too.
        _frame.push(frameMs, frameMs);
        _cpu.push(static_cast<float>(frame.updateTime + frame.renderTime), frameMs);
        _gpu.push(profiler ? static_cast<float>(profiler->frameMilliseconds()) : 0.0f, frameMs);
        _drawCalls.push(static_cast<float>(drawCalls.total), frameMs);
        // Textures and geometry, all live counters since 2026-09-16. Labelled VRAM as upstream
        // labels its `vram.totalUsed`, but a LOWER BOUND: the backends' uniform and storage
        // pools are not tracked, and the texture figure is content size (no driver padding,
        // no GPU-generated mips). The detailed view spells the parts out.
        _vram.push(static_cast<float>(static_cast<double>(vram.tex + vram.vb + vram.ib) * toMb), frameMs);
        recordPassTimings();

        // Smoothed frame rate over the sample window, paired with the same window's mean so the
        // two figures cannot contradict each other.
        const float averageMs = _frame.history.average();
        _fps = averageMs > 0.0001f ? 1000.0f / averageMs : 0.0f;

        _overlay->beginFrame();

        // Bottom-left, inset by upstream's 8 px, following the window through resizes. The panel
        // is not draggable, and it has no title bar: upstream's is a bare rectangle too.
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(
            ImVec2(viewport->WorkPos.x + kInset, viewport->WorkPos.y + viewport->WorkSize.y - kInset),
            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(_detailed ? 0.0f : kCompactWidth, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
        ImGui::SetNextWindowBgAlpha(0.75f);

        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
            | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings;

        if (ImGui::Begin("MiniStats", nullptr, flags)) {
            // A click anywhere on the panel switches views, as a click cycles upstream's sizes.
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                _detailed = !_detailed;
            }

            if (!_detailed) {
                // Upstream's compact size, in its order: the Engine counters (draw calls, then
                // frame), then CPU, GPU and VRAM. Decimal places are upstream's per stat.
                compactRow("Draw calls", _drawCalls.displayed, 0, "");
                compactRow("Frame", _frame.displayed, 1, "ms");
                compactRow("CPU", _cpu.displayed, 1, "ms");
                compactRow("GPU", _gpu.displayed, 1, "ms");
                compactRow("VRAM", _vram.displayed, 1, "MB");
            } else {
                ImGui::Text("%.0f fps   %.2f ms avg", _fps, averageMs);

                plot("Frame", _frame.history, "ms");
                plot("CPU", _cpu.history, "ms");
                plot("GPU", _gpu.history, "ms");

                ImGui::Separator();
                plot("draws", _drawCalls.history, "");
                ImGui::Text("draw calls  %d  (forward %d, skinned %d)",
                    drawCalls.total, drawCalls.forward, drawCalls.skinned);
                if (frame.gsplats > 0) {
                    ImGui::Text("gsplats     %d", frame.gsplats);
                }

                // Still NOT called a VRAM total here, where there is room to say why: the
                // backends' uniform and storage pools are not tracked at all, and the texture
                // figure is content size, so it excludes driver padding and any mip generated
                // on the GPU after creation.
                ImGui::Text("tex+geom    %.1f MB  (tex %.1f, vb %.1f, ib %.1f)",
                    static_cast<double>(vram.tex + vram.vb + vram.ib) * toMb,
                    static_cast<double>(vram.tex) * toMb,
                    static_cast<double>(vram.vb) * toMb,
                    static_cast<double>(vram.ib) * toMb);
                // The shadow/asset/lightmap split, live since the creation sites were
                // tagged with a TexHint. What is left untagged is deliberate — render
                // targets, the post chain, env atlases and probes are neither loaded
                // content nor shadow maps — so the three named buckets do not sum to
                // `tex`, and the remainder is shown rather than left to be inferred
                // from a subtraction that would look like an error.
                const int64_t texOther = static_cast<int64_t>(vram.tex)
                    - static_cast<int64_t>(vram.texAsset)
                    - static_cast<int64_t>(vram.texShadow)
                    - static_cast<int64_t>(vram.texLightmap);
                ImGui::Text("  of which   asset %.1f, shadow %.1f, lightmap %.1f, other %.1f",
                    static_cast<double>(vram.texAsset) * toMb,
                    static_cast<double>(vram.texShadow) * toMb,
                    static_cast<double>(vram.texLightmap) * toMb,
                    static_cast<double>(texOther) * toMb);

                // Deliberately no triangle / material-switch / shader-switch / CPU-breakdown rows:
                // those ApplicationStats fields exist (inherited from upstream's struct shape) but
                // nothing in this engine writes them, so displaying them would print a confident
                // zero rather than a measurement. Add rows here as the counters get instrumented.

                ImGui::Separator();
                if (!profiler) {
                    ImGui::TextUnformatted("GPU passes: not supported on this backend");
                } else if (_passHistories.empty()) {
                    // Timings resolve about two frames after submission, so this shows briefly on
                    // startup rather than indicating a failure.
                    ImGui::TextUnformatted("GPU passes: resolving...");
                } else {
                    ImGui::Text("GPU passes (ms)  total %.2f", profiler->frameMilliseconds());
                    for (const auto& entry : _passHistories) {
                        ImGui::Text("  %-26s %6.3f", entry.name.c_str(), entry.history.latest);
                    }
                }
            }
        }
        ImGui::End();

        _overlay->endFrame();
        _overlay->renderToGPU();
    }
}
