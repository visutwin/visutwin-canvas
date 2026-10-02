// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#include "dualGestureSource.h"

#include "framework/engine.h"
#include "platform/input/touchDevice.h"

namespace visutwin::canvas
{
    namespace
    {
        // Upstream constants.js.
        constexpr auto kDoubleTapThreshold = std::chrono::milliseconds(250);
        constexpr float kDoubleTapVariance = 100.0f;
    }

    DualGestureSource::DualGestureSource(const std::string& layout)
        : InputSource({{"leftInput", 2}, {"rightInput", 2}, {"doubleTap", 1}})
    {
        setLayout(layout);
    }

    DualGestureSource::~DualGestureSource()
    {
        _leftJoystick.up();
        _rightJoystick.up();
        detach();
    }

    void DualGestureSource::setLayout(const std::string& layout)
    {
        if (_layout == layout) {
            return;
        }
        _layout = layout;
        read();
        _pointers.clear();
    }

    void DualGestureSource::onDown(const int64_t id, const float x, const float y)
    {
        const float width = _engine ? static_cast<float>(_engine->canvasSize().first) : 0.0f;
        const bool left = x < width * 0.5f;
        std::erase_if(_pointers, [id](const Pointer& p) { return p.id == id; });
        _pointers.push_back({id, x, y, left});

        const auto now = std::chrono::steady_clock::now();
        const float sqrDist = (_lastX - x) * (_lastX - x) + (_lastY - y) * (_lastY - y);
        if (sqrDist < kDoubleTapVariance && now - _lastTime < kDoubleTapThreshold) {
            deltas.at("doubleTap").append({1.0f});
        }
        _lastX = x;
        _lastY = y;
        _lastTime = now;

        if (left && leftIsJoystick()) {
            fire("joystick:position:left", _leftJoystick.down(x, y));
        }
        if (!left && rightIsJoystick()) {
            fire("joystick:position:right", _rightJoystick.down(x, y));
        }
    }

    void DualGestureSource::onMove(const int64_t id, const float x, const float y)
    {
        Pointer* data = nullptr;
        for (auto& p : _pointers) {
            if (p.id == id) {
                data = &p;
            }
        }
        if (!data) {
            return;
        }
        const float movementX = x - data->x;
        const float movementY = y - data->y;
        data->x = x;
        data->y = y;

        if (data->left) {
            if (leftIsJoystick()) {
                fire("joystick:position:left", _leftJoystick.move(x, y));
            } else {
                deltas.at("leftInput").append({movementX, movementY});
            }
        } else {
            if (rightIsJoystick()) {
                fire("joystick:position:right", _rightJoystick.move(x, y));
            } else {
                deltas.at("rightInput").append({movementX, movementY});
            }
        }
    }

    void DualGestureSource::onUp(const int64_t id)
    {
        const auto it = std::find_if(_pointers.begin(), _pointers.end(), [id](const Pointer& p) { return p.id == id; });
        if (it == _pointers.end()) {
            return;
        }
        const bool left = it->left;
        _pointers.erase(it);
        if (left && leftIsJoystick()) {
            fire("joystick:position:left", _leftJoystick.up());
        }
        if (!left && rightIsJoystick()) {
            fire("joystick:position:right", _rightJoystick.up());
        }
    }

    void DualGestureSource::attach(Engine* engine)
    {
        InputSource::attach(engine);
        TouchDevice* touch = engine ? engine->touch() : nullptr;
        if (!touch) {
            return;
        }
        _handles.push_back(touch->on("touchstart", [this](const TouchEvent& e) {
            for (const Touch& t : e.changed) {
                onDown(t.id, t.x, t.y);
            }
        }));
        _handles.push_back(touch->on("touchmove", [this](const TouchEvent& e) {
            for (const Touch& t : e.changed) {
                onMove(t.id, t.x, t.y);
            }
        }));
        const auto up = [this](const TouchEvent& e) {
            for (const Touch& t : e.changed) {
                onUp(t.id);
            }
        };
        _handles.push_back(touch->on("touchend", up));
        _handles.push_back(touch->on("touchcancel", up));
    }

    void DualGestureSource::detach()
    {
        if (!_engine) {
            return;
        }
        for (const auto& handle : _handles) {
            handle->off();
        }
        _handles.clear();
        _pointers.clear();
        InputSource::detach();
    }

    InputValues DualGestureSource::read()
    {
        deltas.at("leftInput").append({_leftJoystick.value().x, _leftJoystick.value().y});
        deltas.at("rightInput").append({_rightJoystick.value().x, _rightJoystick.value().y});
        return InputSource::read();
    }
}
