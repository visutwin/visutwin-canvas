// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream graphics/wide-line.
//
// One polyline of 96 points following a sine wave, drawn by a WideLineRenderer: the
// width ramps from 4 to 18 pixels along its length and the colour from cyan to pink,
// with round caps and round joins. A hardware line is one pixel wide and takes no
// colour or width per point; this is the primitive that does.
//
// DEVIATION: upstream exposes every setting through its controls panel. Here the
// keys cycle the cap and join styles and toggle the dash pattern, and the rest are
// upstream's initial values.
//
#include <cmath>
#include <memory>
#include <vector>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "scene/constants.h"
#include "scene/graphics/wideLine.h"
#include "scene/graphics/wideLineRenderer.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int kPoints = 96;
    constexpr float kAmplitude = 2.2f;
    constexpr float kFrequency = 1.5f;
    constexpr float kStartWidth = 4.0f;
    constexpr float kEndWidth = 18.0f;
}

class WideLineExample final: public ExampleApp
{
public:
    WideLineExample(): ExampleApp({.title = "Wide Line"}) {}

protected:
    bool create() override
    {
        scene()->setToneMapping(TONEMAP_NONE);

        auto* camera = createCamera(Vector3(12.5f, 4.0f, 12.5f));
        if (auto* comp = camera->findComponent<CameraComponent>();
            comp != nullptr && comp->camera() != nullptr) {
            comp->camera()->setClearColor(Color(0.025f, 0.035f, 0.055f, 1.0f));
            comp->camera()->setNearClip(0.1f);
            comp->camera()->setFarClip(100.0f);
        }
        auto* controls = addOrbitControls(camera, Vector3(0.0f, 0.0f, 0.0f));
        controls->setOrbitDistance(18.0f);
        controls->storeResetState();

        _renderer = std::make_unique<WideLineRenderer>(engine(), device());
        _renderer->setScreenSize(static_cast<float>(windowWidth()), static_cast<float>(windowHeight()));
        _renderer->add(&_line);

        _line.setCap(LineCap::Round);
        _line.setJoin(LineJoin::Round);
        buildLine();

        spdlog::info("C cycles the cap style, J the join style, D toggles dashes.");
        return true;
    }

    void update(float) override
    {
        // The renderer uploads only when something changed, so calling this every
        // frame costs nothing while the line is still.
        if (_renderer) {
            _renderer->update();
        }
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_C:
            _cap = static_cast<LineCap>((static_cast<int>(_cap) + 1) % 3);
            _line.setCap(_cap);
            spdlog::info("[cap] {}", capName(_cap));
            return true;
        case SDLK_J:
            _join = static_cast<LineJoin>((static_cast<int>(_join) + 1) % 3);
            _line.setJoin(_join);
            spdlog::info("[join] {}", joinName(_join));
            return true;
        case SDLK_D:
            _dashed = !_dashed;
            _line.setDash(_dashed ? 0.6f : 0.0f, _dashed ? 0.4f : 0.0f);
            spdlog::info("[dash] {}", _dashed ? "ON" : "OFF");
            return true;
        default:
            return false;
        }
    }

private:
    static const char* capName(const LineCap cap)
    {
        switch (cap) {
        case LineCap::Butt:   return "butt";
        case LineCap::Square: return "square";
        default:              return "round";
        }
    }

    static const char* joinName(const LineJoin join)
    {
        switch (join) {
        case LineJoin::Miter: return "miter";
        case LineJoin::Bevel: return "bevel";
        default:              return "round";
        }
    }

    void buildLine()
    {
        std::vector<float> positions(kPoints * 3);
        std::vector<float> colors(kPoints * 3);
        std::vector<float> widths(kPoints);

        const float startColor[3] = {0.1f, 0.7f, 1.0f};
        const float endColor[3] = {1.0f, 0.15f, 0.45f};

        for (int i = 0; i < kPoints; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kPoints - 1);
            const float angle = t * 6.28318530718f * kFrequency;

            positions[i * 3 + 0] = (t - 0.5f) * 16.0f;
            positions[i * 3 + 1] = std::sin(angle) * kAmplitude;
            positions[i * 3 + 2] = std::cos(angle * 0.5f) * 1.5f;

            widths[i] = kStartWidth + (kEndWidth - kStartWidth) * t;

            for (int c = 0; c < 3; ++c) {
                colors[i * 3 + c] = startColor[c] + (endColor[c] - startColor[c]) * t;
            }
        }

        _line.setPoints(positions, colors, widths);
    }

    std::unique_ptr<WideLineRenderer> _renderer;
    WideLine _line;
    LineCap _cap = LineCap::Round;
    LineJoin _join = LineJoin::Round;
    bool _dashed = false;
};

VISUTWIN_EXAMPLE_MAIN(WideLineExample)
