// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#include "miniStats.h"

#include <algorithm>

#include <SDL3/SDL.h>

#include "imgui.h"

#include "framework/applicationStats.h"
#include "framework/engine.h"
#include "platform/graphics/graphicsDevice.h"
#include "viz/overlay/imguiOverlay.h"

namespace visutwin::canvas
{
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

        for (const auto& timing : profiler->passTimings()) {
            const auto existing = std::ranges::find_if(_passHistories,
                [&timing](const PassHistory& entry) { return entry.name == timing.name; });

            if (existing != _passHistories.end()) {
                existing->history.push(static_cast<float>(timing.milliseconds));
            } else {
                PassHistory entry;
                entry.name = timing.name;
                entry.history.push(static_cast<float>(timing.milliseconds));
                _passHistories.push_back(std::move(entry));
            }
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

        // CPU frame time is measured here rather than read from ApplicationStats: this hook runs
        // exactly once per rendered frame, and it keeps the HUD working whichever loop drives the
        // engine (upstream's CpuTimer measures it the same way).
        const uint64_t counter = SDL_GetPerformanceCounter();
        const uint64_t frequency = SDL_GetPerformanceFrequency();
        if (_lastCounter != 0 && frequency != 0) {
            const double elapsedMs =
                static_cast<double>(counter - _lastCounter) * 1000.0 / static_cast<double>(frequency);
            _cpuFrame.push(static_cast<float>(elapsedMs));

            // Smoothed frame rate over the sample window.
            const float averageMs = _cpuFrame.average();
            _fps = averageMs > 0.0001f ? 1000.0f / averageMs : 0.0f;
        }
        _lastCounter = counter;

        const auto& frame = stats->frame();
        const auto& drawCalls = stats->drawCalls();
        const auto& profiler = device->gpuProfiler();

        _gpuFrame.push(profiler ? static_cast<float>(profiler->frameMilliseconds()) : 0.0f);
        _drawCalls.push(static_cast<float>(drawCalls.total));
        recordPassTimings();

        _overlay->beginFrame();

        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.75f);
        if (ImGui::Begin("MiniStats", nullptr,
                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize)) {

            // Both figures come from the same sample window, so they stay consistent with each
            // other (pairing fps with the latest frame's ms reads as a contradiction).
            ImGui::Text("%.0f fps   %.2f ms avg", _fps, _cpuFrame.average());
            ImGui::Checkbox("details", &_expanded);

            plot("CPU", _cpuFrame, "ms");
            plot("GPU", _gpuFrame, "ms");

            if (_expanded) {
                ImGui::Separator();
                plot("draws", _drawCalls, "");
                ImGui::Text("draw calls  %d  (forward %d, skinned %d)",
                    drawCalls.total, drawCalls.forward, drawCalls.skinned);
                if (frame.gsplats > 0) {
                    ImGui::Text("gsplats     %d", frame.gsplats);
                }

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
