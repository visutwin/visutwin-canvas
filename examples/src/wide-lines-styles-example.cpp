// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/wide-lines-styles.
//
// Eight wavy lines of 216 points each, one row per style, all drawn by one WideLineRenderer
// in a single instanced draw. Every line runs 16 units across, waves in y and z, and fades
// from cyan to pink along its length. From the top row down:
//   1. 3 px, butt caps, miter joins
//   2. 3 px with both ends widening from 0 to 36 px over the first and last 6%: a double arrow
//   3. 10 px, round caps, round joins
//   4. 10 px, square caps, bevel joins
//   5. 7 px, butt caps, dashes 0.65 long with 0.35 gaps
//   6. 10 px, round caps, dashes of 0.001 with 0.5 gaps: a dotted line of round dots
//   7. variable width, 2 px at the ends swelling to 14 in the middle, round caps and joins
//   8. 16 px, round caps and joins, tapering to a point over the last 18%: an arrow
// Each line also carries world-space widths, 0.03 units per pixel width, for the world toggle.
// The camera orbits the origin from (0, 1, 24).
//
// Keys standing in for upstream's controls panel:
//   W  world-space widths on / off (off: widths in screen pixels)
//
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "platform/input/inputConstants.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"
#include "scene/graphics/wideLine.h"
#include "scene/graphics/wideLineRenderer.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int kPointCount = 216;

    struct LineStyle
    {
        float width = 0.0f;
        LineCap cap = LineCap::Butt;
        LineJoin join = LineJoin::Miter;
        // Unset: the row's own phase, row * 0.35
        std::optional<double> phase;
        float dashLength = 0.0f;
        float gapLength = 0.0f;
        bool doubleArrow = false;
        bool variable = false;
        bool arrow = false;
    };

    const std::array<LineStyle, 8> kStyles = {{
        {.width = 3.0f, .cap = LineCap::Butt, .join = LineJoin::Miter, .phase = 0.0},
        {.width = 3.0f, .cap = LineCap::Butt, .join = LineJoin::Miter, .phase = 0.0, .doubleArrow = true},
        {.width = 10.0f, .cap = LineCap::Round, .join = LineJoin::Round},
        {.width = 10.0f, .cap = LineCap::Square, .join = LineJoin::Bevel},
        {.width = 7.0f, .cap = LineCap::Butt, .join = LineJoin::Miter, .dashLength = 0.65f, .gapLength = 0.35f},
        {.width = 10.0f, .cap = LineCap::Round, .join = LineJoin::Miter, .dashLength = 0.001f, .gapLength = 0.5f},
        {.width = 12.0f, .cap = LineCap::Round, .join = LineJoin::Round, .variable = true},
        {.width = 16.0f, .cap = LineCap::Round, .join = LineJoin::Round, .arrow = true},
    }};
}

class WideLinesStylesExample final: public ExampleApp
{
public:
    WideLinesStylesExample(): ExampleApp({.title = "Wide Lines Styles"}) {}

protected:
    bool create() override
    {
        auto* camera = createCamera(Vector3(0.0f, 1.0f, 24.0f));
        camera->setName("Camera");
        if (auto* component = camera->findComponent<CameraComponent>(); component && component->camera()) {
            component->camera()->setClearColor(Color(0.02f, 0.025f, 0.04f, 1.0f));
            component->camera()->setNearClip(0.1f);
            component->camera()->setFarClip(100.0f);
        }
        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 0.0f, 0.0f))) {
            controls->setEnableFly(false);
            controls->setPitchRange(Vector2(-80.0f, 80.0f));
            controls->setZoomRange(Vector2(10.0f, 45.0f));
            controls->storeResetState();
        }

        _renderer = std::make_unique<WideLineRenderer>(engine(), device());
        _renderer->setLayers({LAYERID_IMMEDIATE});
        syncScreenSize();

        for (size_t row = 0; row < kStyles.size(); ++row) {
            const LineStyle& style = kStyles[row];
            std::vector<float> positions(kPointCount * 3);
            std::vector<float> colors(kPointCount * 3);
            std::vector<float> widths(kPointCount);
            const double r = static_cast<double>(row);
            const double phase = style.phase.value_or(r * 0.35);

            for (int point = 0; point < kPointCount; ++point) {
                const double t = static_cast<double>(point) / (kPointCount - 1);
                const int p = point * 3;
                positions[p] = static_cast<float>((t - 0.5) * 16.0);
                positions[p + 1] = static_cast<float>(5.7 - r * 1.9 +
                    std::sin(t * std::numbers::pi * 3.0 + phase) * 0.5);
                positions[p + 2] = static_cast<float>(std::cos(t * std::numbers::pi * 2.0 + r) * 0.4);

                colors[p] = static_cast<float>(0.15 + 0.75 * t);
                colors[p + 1] = static_cast<float>(0.8 - r * 0.08);
                colors[p + 2] = static_cast<float>(1.0 - 0.7 * t + r * 0.04);

                double width = style.variable ? 2.0 + std::sin(t * std::numbers::pi) * style.width : style.width;
                if (style.doubleArrow) {
                    const double edge = std::min(t, 1.0 - t);
                    width = edge < 0.06 ? (36.0 * edge) / 0.06 : style.width;
                }
                if (style.arrow && t > 0.82) {
                    width = (style.width * (1.0 - t)) / 0.18;
                }
                widths[point] = static_cast<float>(width);
            }

            auto& entry = _lines.emplace_back();
            entry.line = std::make_unique<WideLine>();
            entry.line->setPoints(positions, colors, widths);
            entry.line->setCap(style.cap);
            entry.line->setJoin(style.join);
            entry.line->setDash(style.dashLength, style.gapLength);
            _renderer->add(entry.line.get());

            entry.worldWidths.resize(kPointCount);
            for (int point = 0; point < kPointCount; ++point) {
                entry.worldWidths[point] = widths[point] * 0.03f;
            }
            entry.screenWidths = std::move(widths);
        }

        updateWidthUnits();
        _renderer->update();

        spdlog::info("W toggles world-space widths");
        return true;
    }

    void update(float) override
    {
        if (const auto* keyboard = engine()->keyboard(); keyboard && keyboard->wasPressed(Key::W)) {
            _worldSpaceWidths = !_worldSpaceWidths;
            updateWidthUnits();
            spdlog::info("[world-space widths] {}", _worldSpaceWidths ? "ON" : "OFF");
        }
        syncScreenSize();
        if (_renderer) {
            _renderer->update();
        }
    }

    void destroy() override
    {
        _renderer.reset();
    }

private:
    struct StyledLine
    {
        std::unique_ptr<WideLine> line;
        std::vector<float> screenWidths;
        std::vector<float> worldWidths;
    };

    void updateWidthUnits()
    {
        _renderer->setWidthUnits(_worldSpaceWidths ? LineWidthUnits::World : LineWidthUnits::Pixels);
        for (auto& entry : _lines) {
            entry.line->setWidths(_worldSpaceWidths ? entry.worldWidths : entry.screenWidths);
        }
    }

    // The renderer expands pixel widths against the back buffer size; it is told only
    // when that size changes, since telling it forces a rebuild.
    void syncScreenSize()
    {
        if (!_renderer || !device()) {
            return;
        }
        const auto size = device()->size();
        if (size != _screenSize) {
            _screenSize = size;
            _renderer->setScreenSize(static_cast<float>(size.first), static_cast<float>(size.second));
        }
    }

    std::unique_ptr<WideLineRenderer> _renderer;
    std::vector<StyledLine> _lines;
    std::pair<int, int> _screenSize{0, 0};
    bool _worldSpaceWidths = false;
};

VISUTWIN_EXAMPLE_MAIN(WideLinesStylesExample)
