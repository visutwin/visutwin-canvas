// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream debug/wire-shapes.
//
// Every WireRenderer shape in a 6 x 3 wall of cells facing the camera, each animated at its own
// phase and drawn in its own slowly cycling colour: line, lines (a colour per point), linesPacked
// (spokes fading from white at the hub), polyline, loop, box (an oriented box through the
// renderer's transform), boxMinMax, sphere, circle, cylinder, capsule, cone, plane (turned by a
// transform), point, arrow, axes, frustum (a projection placed by a transform) and light (a spot
// light, drawn in the cell's colour). The camera orbits the wall.
//
// Keys standing in for upstream's controls panel:
//   [ / ]  circle segments -/+ 1 (20 to begin with)
//   D      depth test on / off
//   A      animation on / off
//
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>
#include <vector>

#include "../exampleApp.h"
#include "core/shape/boundingBox.h"
#include "extras/script/cameraControls.h"
#include "framework/components/light/lightComponent.h"
#include "framework/extras/wireRenderer.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int kSpokes = 8;
    constexpr int kColumns = 6;
    constexpr float kSpacing = 4.4f;
}

class WireShapesExample final : public ExampleApp
{
public:
    WireShapesExample() : ExampleApp({.title = "Wire Shapes"}) {}

protected:
    bool create() override
    {
        auto* camera = createCamera(Vector3(0.0f, 0.0f, 26.0f));
        if (auto* component = camera->findComponent<CameraComponent>(); component && component->camera()) {
            component->camera()->setClearColor(Color(0.02f, 0.025f, 0.04f, 1.0f));
            component->camera()->setNearClip(0.1f);
            component->camera()->setFarClip(200.0f);
        }
        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 0.0f, 0.0f))) {
            controls->setEnableFly(false);
            controls->setPitchRange(Vector2(-80.0f, 80.0f));
            controls->setZoomRange(Vector2(8.0f, 60.0f));
            controls->storeResetState();
        }

        _wire.emplace(scene().get());

        // A spot light, so the light shape has something real to draw
        _spot = new Entity();
        _spot->setName("spot");
        _spot->setEngine(engine());
        _spotLight = static_cast<LightComponent*>(_spot->addComponent<LightComponent>());
        _spotLight->setType(LightType::LIGHTTYPE_SPOT);
        _spotLight->setColor(Color(1.0f, 0.7f, 0.2f, 1.0f));
        _spotLight->setRange(2.6f);
        _spotLight->setInnerConeAngle(10.0f);
        _spotLight->setOuterConeAngle(30.0f);
        root()->addChild(_spot);

        buildShapes();
        spdlog::info("Keys: [ ] segments, D depth test, A animate");
        return true;
    }

    void update(const float dt) override
    {
        if (const auto* keyboard = engine()->keyboard()) {
            if (keyboard->wasPressed(Key::LeftBracket)) _segments = std::max(3, _segments - 1);
            if (keyboard->wasPressed(Key::RightBracket)) _segments = std::min(64, _segments + 1);
            if (keyboard->wasPressed(Key::D)) _depthTest = !_depthTest;
            if (keyboard->wasPressed(Key::A)) _animate = !_animate;
        }
        if (_animate) {
            _time += dt;
        }
        _wire->setSegments(_segments);
        _wire->setDepthTest(_depthTest);

        const int rows = (static_cast<int>(_shapes.size()) + kColumns - 1) / kColumns;
        for (size_t i = 0; i < _shapes.size(); ++i) {
            const int column = static_cast<int>(i) % kColumns;
            const int row = static_cast<int>(i) / kColumns;
            // Laid out as a wall facing the camera, so every cell is the same size on screen
            const Vector3 cell((static_cast<float>(column) - (kColumns - 1) * 0.5f) * kSpacing,
                ((rows - 1) * 0.5f - static_cast<float>(row)) * kSpacing, 0.0f);
            // Each cell runs the same animation at its own phase
            const float phase = static_cast<float>(i) * 0.7f;
            const float pulse = 0.7f + std::sin(_time * 1.5f + phase) * 0.3f;
            _cellColor = Color(0.5f + 0.5f * std::sin(phase + _time * 0.4f),
                0.5f + 0.5f * std::sin(phase + _time * 0.4f + 2.1f),
                0.5f + 0.5f * std::sin(phase + _time * 0.4f + 4.2f), 1.0f);
            // The transform is per shape, so it is cleared before every cell
            _wire->clearTransform();
            _wire->setColor(_cellColor);
            _shapes[i](cell, pulse, _time);
        }
    }

private:
    using Shape = std::function<void(const Vector3&, float, float)>;

    void buildShapes()
    {
        WireRenderer& wire = *_wire;
        const Vector3 one(1.0f, 1.0f, 1.0f);
        _shapes = {
            // line
            [&wire](const Vector3& c, const float pulse, float) {
                wire.line(c - Vector3(0.0f, pulse, 0.0f), c + Vector3(0.0f, pulse, 0.0f));
            },
            // lines: per-point colours interpolate along each segment, a gradient
            [&wire](const Vector3& c, const float pulse, float) {
                const std::array<Vector3, 4> points = {
                    c + Vector3(-pulse, -pulse, 0.0f), c + Vector3(pulse, pulse, 0.0f),
                    c + Vector3(-pulse, pulse, 0.0f), c + Vector3(pulse, -pulse, 0.0f)};
                const Color red(1.0f, 0.2f, 0.2f, 1.0f);
                const Color blue(0.2f, 0.5f, 1.0f, 1.0f);
                const std::array<Color, 4> colors = {red, blue, blue, red};
                wire.lines(points, colors);
            },
            // linesPacked: each spoke fades from white at the hub to its own tip colour
            [this, &wire](const Vector3& c, const float pulse, const float time) {
                for (int spoke = 0; spoke < kSpokes; ++spoke) {
                    const float angle = static_cast<float>(spoke) / kSpokes * 6.28318530718f + time * 0.6f;
                    const int hub = spoke * 2;
                    const int tip = hub + 1;
                    _packedPositions[hub * 3] = c.getX();
                    _packedPositions[hub * 3 + 1] = c.getY();
                    _packedPositions[hub * 3 + 2] = c.getZ();
                    _packedPositions[tip * 3] = c.getX() + std::sin(angle) * pulse * 1.4f;
                    _packedPositions[tip * 3 + 1] = c.getY() + std::cos(angle) * pulse * 1.4f;
                    _packedPositions[tip * 3 + 2] = c.getZ();
                    for (int k = 0; k < 4; ++k) {
                        _packedColors[hub * 4 + k] = 1.0f;
                    }
                    _packedColors[tip * 4] = 0.5f + 0.5f * std::sin(angle);
                    _packedColors[tip * 4 + 1] = 0.5f + 0.5f * std::sin(angle + 2.1f);
                    _packedColors[tip * 4 + 2] = 0.5f + 0.5f * std::sin(angle + 4.2f);
                    _packedColors[tip * 4 + 3] = 1.0f;
                }
                wire.linesPacked(_packedPositions, _packedColors);
            },
            // polyline
            [&wire](const Vector3& c, const float pulse, const float time) {
                std::array<Vector3, 16> points;
                for (int i = 0; i < 16; ++i) {
                    const float t = static_cast<float>(i) / 15.0f;
                    points[i] = c + Vector3((t - 0.5f) * 2.4f,
                        std::sin(t * 6.28318530718f + time * 2.0f) * pulse * 0.8f, 0.0f);
                }
                wire.polyline(points);
            },
            // loop
            [&wire](const Vector3& c, const float pulse, const float time) {
                std::array<Vector3, 7> points;
                for (int i = 0; i < 7; ++i) {
                    const float angle = static_cast<float>(i) / 7.0f * 6.28318530718f + time * 0.5f;
                    points[i] = c + Vector3(std::sin(angle) * pulse * 1.2f, std::cos(angle) * pulse * 1.2f, 0.0f);
                }
                wire.loop(points);
            },
            // box: an oriented box, by a rotation in the renderer's transform
            [&wire, one](const Vector3& c, const float pulse, const float time) {
                wire.setTransform(Matrix4::trs(c, Quaternion::fromEulerAngles(time * 30.0f, time * 45.0f, 0.0f), one));
                wire.box(BoundingBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(pulse, pulse * 0.7f, pulse * 0.5f)));
            },
            // boxMinMax
            [&wire](const Vector3& c, const float pulse, float) {
                wire.boxMinMax(c - Vector3(pulse, pulse, pulse), c + Vector3(pulse, pulse, pulse));
            },
            // sphere
            [&wire](const Vector3& c, const float pulse, float) { wire.sphere(c, pulse); },
            // circle
            [&wire](const Vector3& c, const float pulse, const float time) {
                wire.circle(c, Vector3(std::sin(time), 1.0f, std::cos(time * 0.7f)).normalized(), pulse * 1.2f);
            },
            // cylinder
            [&wire](const Vector3& c, const float pulse, const float time) {
                const Vector3 axis = Vector3(std::sin(time * 0.6f), 1.0f, 0.0f).normalized() * 1.1f;
                wire.cylinder(c - axis, c + axis, pulse * 0.7f);
            },
            // capsule
            [&wire](const Vector3& c, const float pulse, const float time) {
                const Vector3 axis = Vector3(0.0f, 1.0f, std::sin(time * 0.8f) * 0.6f).normalized() * 0.8f;
                wire.capsule(c - axis, c + axis, pulse * 0.6f);
            },
            // cone
            [&wire](const Vector3& c, const float pulse, const float time) {
                const Vector3 axis = Vector3(std::sin(time * 0.9f) * 0.5f, -1.0f, 0.0f).normalized();
                wire.cone(c + Vector3(0.0f, 1.2f, 0.0f), axis, 12.0f + pulse * 28.0f, 2.4f);
            },
            // plane: a square has a definite orientation, so it is turned by a transform
            [&wire, one](const Vector3& c, const float pulse, const float time) {
                wire.setTransform(Matrix4::trs(c,
                    Quaternion::fromEulerAngles(time * 25.0f, time * 18.0f, time * 12.0f), one));
                wire.plane(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), pulse * 2.4f);
            },
            // point
            [&wire](const Vector3& c, const float pulse, float) { wire.point(c, pulse * 3.4f); },
            // arrow
            [&wire](const Vector3& c, const float pulse, const float time) {
                const Vector3 axis = Vector3(std::sin(time * 1.3f), std::cos(time * 0.9f), std::sin(time * 0.5f))
                    .normalized() * (pulse * 1.6f);
                wire.arrow(c - axis, c + axis);
            },
            // axes: always red, green and blue; the matrix is passed directly, not as the
            // renderer's transform, which would apply it twice
            [&wire, one](const Vector3& c, const float pulse, const float time) {
                wire.axes(Matrix4::trs(c, Quaternion::fromEulerAngles(time * 20.0f, time * 35.0f, time * 15.0f), one),
                    pulse * 2.0f);
            },
            // frustum: a bare projection sits at the origin looking down -Z, so the renderer's
            // transform moves it into the cell
            [&wire, one](const Vector3& c, const float pulse, const float time) {
                wire.setTransform(Matrix4::trs(c, Quaternion::fromEulerAngles(-20.0f, time * 25.0f, 0.0f), one));
                wire.frustum(Matrix4::perspective(30.0f + pulse * 40.0f, 1.4f, 0.5f, 3.0f));
            },
            // light: a light shines along the negative Y axis of its entity
            [this, &wire](const Vector3& c, const float pulse, const float time) {
                _spot->setLocalPosition(c.getX(), c.getY() + 1.3f, c.getZ());
                _spot->setLocalEulerAngles(std::sin(time * 0.8f) * 25.0f, 0.0f, std::cos(time * 0.6f) * 25.0f);
                _spotLight->setOuterConeAngle(12.0f + pulse * 28.0f);
                _spotLight->setColor(_cellColor);
                wire.light(_spotLight);
            },
        };
    }

    std::optional<WireRenderer> _wire;
    std::vector<Shape> _shapes;
    std::array<float, kSpokes * 2 * 3> _packedPositions{};
    std::array<float, kSpokes * 2 * 4> _packedColors{};
    Entity* _spot = nullptr;
    LightComponent* _spotLight = nullptr;
    Color _cellColor = Color(1.0f, 1.0f, 1.0f, 1.0f);
    float _time = 0.0f;
    int _segments = 20;
    bool _depthTest = true;
    bool _animate = true;
};

VISUTWIN_EXAMPLE_MAIN(WireShapesExample)
