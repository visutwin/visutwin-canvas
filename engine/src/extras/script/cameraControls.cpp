// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 28.12.2025
//
#include "extras/script/cameraControls.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <spdlog/spdlog.h>

#include "core/math/quaternion.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"

namespace visutwin::canvas
{
    namespace
    {
        void applyDeadZone(std::vector<float>& stick, const float low, const float high)
        {
            const float mag = std::sqrt(stick[0] * stick[0] + stick[1] * stick[1]);
            if (mag < low) {
                stick[0] = 0.0f;
                stick[1] = 0.0f;
                return;
            }
            const float scale = (mag - low) / (high - low);
            stick[0] *= scale / mag;
            stick[1] *= scale / mag;
        }

        Vector3 normalizedOrZero(const Vector3& v)
        {
            const float len = v.length();
            return len > 0.0f ? v * (1.0f / len) : Vector3(0.0f);
        }

        void appendVector(InputDelta& delta, const Vector3& v)
        {
            delta.append({v.getX(), v.getY(), v.getZ()});
        }
    }

    CameraControls::CameraControls()
    {
        // Upstream's constructor: orbit zooms down to 0.01 with no upper limit.
        _orbitController.setZoomRange(Vector2(0.01f, std::numeric_limits<float>::infinity()));

        _flyMobileInput.on("joystick:position:left", [this](const std::array<float, 4>& p) {
            if (_mode != Mode::FLY || !entity() || !entity()->engine()) {
                return;
            }
            entity()->engine()->fire(_joystickEventName + ":left", p[0], p[1], p[2], p[3]);
        });
        _flyMobileInput.on("joystick:position:right", [this](const std::array<float, 4>& p) {
            if (_mode != Mode::FLY || !entity() || !entity()->engine()) {
                return;
            }
            entity()->engine()->fire(_joystickEventName + ":right", p[0], p[1], p[2], p[3]);
        });

        // Inputs gathered while disabled are discarded, as upstream's 'state' handler.
        on("state", [this](bool) { discardInputs(); });
    }

    CameraControls::~CameraControls()
    {
        _desktopInput.destroy();
        _orbitMobileInput.destroy();
        _flyMobileInput.destroy();
        _gamepadInput.destroy();
    }

    void CameraControls::discardInputs()
    {
        _desktopInput.read();
        _orbitMobileInput.read();
        _flyMobileInput.read();
        _gamepadInput.read();
    }

    bool CameraControls::ensureSetup()
    {
        if (_ready) {
            return true;
        }
        Entity* owner = entity();
        if (!owner) {
            return false;
        }
        _camera = owner->findComponent<CameraComponent>();
        if (!_camera) {
            spdlog::error("CameraControls: camera component not found");
            return false;
        }
        _ready = true;

        _pose.look(owner->position(), Vector3(0.0f));
        setMode(Mode::ORBIT);
        return true;
    }

    void CameraControls::initialize()
    {
        ensureSetup();
    }

    void CameraControls::setMode(Mode mode)
    {
        if (_enableFly && !_enableOrbit) {
            mode = Mode::FLY;
        } else if (!_enableFly && _enableOrbit) {
            mode = Mode::ORBIT;
        } else if (!_enableFly && !_enableOrbit) {
            spdlog::warn("CameraControls: both fly and orbit modes are disabled");
            return;
        }

        if (_hasMode && _mode == mode) {
            return;
        }
        _mode = mode;
        _hasMode = true;

        if (_controller) {
            _controller->detach();
        }
        switch (_mode) {
        case Mode::ORBIT: _controller = &_orbitController; break;
        case Mode::FLY: _controller = &_flyController; break;
        case Mode::FOCUS: _controller = &_focusController; break;
        }
        _controller->attach(_pose, false);
    }

    void CameraControls::setEnableFly(const bool enable)
    {
        _enableFly = enable;
        if (!_enableFly && _hasMode && _mode == Mode::FLY) {
            setMode(Mode::ORBIT);
        }
    }

    void CameraControls::setEnableOrbit(const bool enable)
    {
        _enableOrbit = enable;
        if (!_enableOrbit && _hasMode && _mode == Mode::ORBIT) {
            setMode(Mode::FLY);
        }
    }

    void CameraControls::setRotateDamping(const float damping)
    {
        _flyController.rotateDamping = damping;
        _orbitController.rotateDamping = damping;
    }

    void CameraControls::setZoomRange(const Vector2& range)
    {
        _zoomRange.x = range.x;
        _zoomRange.y = range.y <= range.x ? std::numeric_limits<float>::infinity() : range.y;
        _orbitController.setZoomRange(_zoomRange);
    }

    void CameraControls::setPitchRange(const Vector2& range)
    {
        _pitchRange = Vector2(std::clamp(range.x, -360.0f, 360.0f), std::clamp(range.y, -360.0f, 360.0f));
        _flyController.setPitchRange(_pitchRange);
        _orbitController.setPitchRange(_pitchRange);
    }

    void CameraControls::setYawRange(const Vector2& range)
    {
        _yawRange = Vector2(std::clamp(range.x, -360.0f, 360.0f), std::clamp(range.y, -360.0f, 360.0f));
        _flyController.setYawRange(_yawRange);
        _orbitController.setYawRange(_yawRange);
    }

    void CameraControls::setMobileInputLayout(const std::string& layout)
    {
        const auto side = [](const std::string_view s) { return s == "joystick" || s == "touch"; };
        const size_t dash = layout.find('-');
        if (dash == std::string::npos || !side(std::string_view(layout).substr(0, dash)) ||
            !side(std::string_view(layout).substr(dash + 1))) {
            spdlog::warn("CameraControls: invalid mobile input layout: {}", layout);
            return;
        }
        _flyMobileInput.setLayout(layout);
    }

    void CameraControls::setFocusPoint(const Vector3& point)
    {
        if (!ensureSetup()) {
            return;
        }
        const Vector3 position = entity()->position();
        _startZoomDist = position.distance(point);
        _controller->attach(_pose.look(position, point), false);
        // Upstream moves the camera on the next update; doing it now keeps a frame
        // rendered before that update (a screenshot of frame 0) on the new view.
        applyPose();
    }

    Vector3 CameraControls::cameraForward() const
    {
        Entity* owner = entity();
        return owner->rotation() * Vector3(0.0f, 0.0f, -1.0f);
    }

    void CameraControls::focus(const Vector3& point, const bool resetZoom)
    {
        if (!ensureSetup()) {
            return;
        }
        focus(point, resetZoom ? _startZoomDist : entity()->position().distance(point));
    }

    void CameraControls::focus(const Vector3& point, const float distance)
    {
        if (!ensureSetup()) {
            return;
        }
        setMode(Mode::FOCUS);
        const Vector3 position = cameraForward() * -distance + point;
        Pose pose;
        _controller->attach(pose.look(position, point));
    }

    void CameraControls::look(const Vector3& point, const bool resetZoom)
    {
        if (!ensureSetup()) {
            return;
        }
        setMode(Mode::FOCUS);
        const Vector3 cameraPosition = entity()->position();
        const Vector3 position = resetZoom
            ? normalizedOrZero(cameraPosition - point) * _startZoomDist + point
            : cameraPosition;
        Pose pose;
        _controller->attach(pose.look(position, point));
    }

    void CameraControls::reset(const Vector3& focusPoint, const Vector3& position)
    {
        if (!ensureSetup()) {
            return;
        }
        setMode(Mode::FOCUS);
        Pose pose;
        _controller->attach(pose.look(position, focusPoint));
    }

    void CameraControls::setOrbitDistance(const float distance)
    {
        if (!ensureSetup()) {
            return;
        }
        const Vector3 focusPoint = _pose.getFocus();
        const Vector3 position = cameraForward() * -distance + focusPoint;
        _controller->attach(_pose.look(position, focusPoint), false);
        applyPose();
    }

    void CameraControls::storeResetState()
    {
        if (!ensureSetup()) {
            return;
        }
        _resetFocus = _pose.getFocus();
        _resetPosition = _pose.position;
        _hasResetState = true;
    }

    void CameraControls::reset()
    {
        if (_hasResetState) {
            reset(_resetFocus, _resetPosition);
        }
    }

    void CameraControls::applyPose()
    {
        Entity* owner = entity();
        owner->setPosition(_pose.position);
        owner->setRotation(Quaternion::fromEulerAngles(_pose.angles.getX(), _pose.angles.getY(), _pose.angles.getZ()));

        if (_autoFarClip && _camera && _camera->camera()) {
            const float desiredFar = std::max(_pose.distance * _farClipScale, _farClipMin);
            if (_camera->camera()->farClip() < desiredFar) {
                _camera->camera()->setFarClip(desiredFar);
            }
        }
    }

    Vector3 CameraControls::screenToWorld(const float dx, const float dy, const float dz) const
    {
        const Camera* camera = _camera ? _camera->camera() : nullptr;
        Engine* engine = entity() ? entity()->engine() : nullptr;
        if (!camera || !engine) {
            return Vector3(0.0f);
        }
        const auto [width, height] = engine->canvasSize();
        if (width <= 0 || height <= 0) {
            return Vector3(0.0f);
        }

        // Deltas into device coordinates, then scaled by the half size of the view
        // at the given distance.
        const float nx = -(dx / static_cast<float>(width)) * 2.0f;
        const float ny = (dy / static_cast<float>(height)) * 2.0f;
        const float aspect = camera->aspectRatio();
        float halfX;
        float halfY;
        if (camera->projection() == ProjectionType::Perspective) {
            const float halfSlice = dz * std::tan(0.5f * camera->fov() * DEG_TO_RAD);
            if (camera->horizontalFov()) {
                halfX = halfSlice;
                halfY = halfSlice / aspect;
            } else {
                halfX = halfSlice * aspect;
                halfY = halfSlice;
            }
        } else {
            halfX = camera->orthoHeight() * aspect;
            halfY = camera->orthoHeight();
        }
        return Vector3(nx * halfX, ny * halfY, 0.0f);
    }

    void CameraControls::update(float dt)
    {
        if (!ensureSetup()) {
            return;
        }
        if (!_sourcesAttached) {
            if (Engine* engine = entity()->engine()) {
                _desktopInput.attach(engine);
                _orbitMobileInput.attach(engine);
                _flyMobileInput.attach(engine);
                _gamepadInput.attach(engine);
                _sourcesAttached = true;
            }
        }

        dt = std::min(dt, 0.1f);

        using KC = KeyboardMouseSource;
        InputValues desktop = _desktopInput.read();
        InputValues orbitMobile = _orbitMobileInput.read();
        InputValues flyMobile = _flyMobileInput.read();
        InputValues gamepad = _gamepadInput.read();

        const auto& key = desktop["key"];
        const auto& button = desktop["button"];
        const auto& mouse = desktop["mouse"];
        const auto& wheel = desktop["wheel"];
        const auto& touch = orbitMobile["touch"];
        const auto& pinch = orbitMobile["pinch"];
        const auto& count = orbitMobile["count"];
        const auto& leftInput = flyMobile["leftInput"];
        const auto& rightInput = flyMobile["rightInput"];
        auto& leftStick = gamepad["leftStick"];
        auto& rightStick = gamepad["rightStick"];

        applyDeadZone(leftStick, _gamepadDeadZone.x, _gamepadDeadZone.y);
        applyDeadZone(rightStick, _gamepadDeadZone.x, _gamepadDeadZone.y);

        // state
        const auto k = [&key](const int code) { return key[static_cast<size_t>(code)]; };
        _state.axis += Vector3(
            (k(KC::letter('D')) - k(KC::letter('A'))) + (k(KC::RIGHT) - k(KC::LEFT)),
            k(KC::letter('E')) - k(KC::letter('Q')),
            (k(KC::letter('W')) - k(KC::letter('S'))) + (k(KC::UP) - k(KC::DOWN)));
        for (size_t i = 0; i < 3; ++i) {
            _state.mouse[i] += button[i];
        }
        _state.shift += k(KC::SHIFT);
        _state.ctrl += k(KC::CTRL);
        _state.touches += count[0];

        if (button[0] == 1.0f || button[1] == 1.0f || wheel[0] != 0.0f) {
            setMode(Mode::ORBIT);
        } else if (button[2] == 1.0f || _state.axis.length() > 0.0f) {
            setMode(Mode::FLY);
        }

        const float orbit = _mode == Mode::ORBIT ? 1.0f : 0.0f;
        const float fly = _mode == Mode::FLY ? 1.0f : 0.0f;
        const float twoFingers = _state.touches > 1.0f ? 1.0f : 0.0f;
        const float desktopPan = (_state.shift != 0.0f || _state.mouse[1] != 0.0f) ? 1.0f : 0.0f;
        const bool mobileJoystick = _flyMobileInput.layout().ends_with("joystick");
        const float pan = _enablePan ? 1.0f : 0.0f;

        // rate-based multipliers (keyboard, gamepad, virtual joystick)
        const float moveMult = (_state.shift != 0.0f ? _moveFastSpeed
            : _state.ctrl != 0.0f ? _moveSlowSpeed : _moveSpeed) * dt;
        const float rotateJoystickMult = _rotateSpeed * _rotateJoystickSens * 60.0f * dt;

        // delta-based multipliers (mouse, touch, wheel)
        const float rotateDeltaMult = _rotateSpeed;
        const float zoomDeltaMult = _zoomSpeed;
        const float zoomTouchDeltaMult = _zoomSpeed * _zoomPinchSens;

        InputDelta& move = _frame.delta("move");
        InputDelta& rotate = _frame.delta("rotate");

        // desktop move
        Vector3 v = normalizedOrZero(_state.axis) * (fly * moveMult);
        v += screenToWorld(mouse[0], mouse[1], _pose.distance) * (orbit * desktopPan * pan);
        v += Vector3(0.0f, 0.0f, wheel[0]) * (orbit * zoomDeltaMult);
        appendVector(move, v);

        // desktop rotate
        appendVector(rotate, Vector3(mouse[0], mouse[1], 0.0f) * ((1.0f - orbit * desktopPan) * rotateDeltaMult));

        // mobile move
        v = Vector3(leftInput[0], 0.0f, -leftInput[1]) * (fly * moveMult);
        v += screenToWorld(touch[0], touch[1], _pose.distance) * (orbit * twoFingers * pan);
        v += Vector3(0.0f, 0.0f, pinch[0]) * (orbit * twoFingers * zoomTouchDeltaMult);
        appendVector(move, v);

        // mobile rotate
        v = Vector3(touch[0], touch[1], 0.0f) * (orbit * (1.0f - twoFingers) * rotateDeltaMult);
        v += Vector3(rightInput[0], rightInput[1], 0.0f) * (fly * (mobileJoystick ? rotateJoystickMult : rotateDeltaMult));
        appendVector(rotate, v);

        // gamepad move
        appendVector(move, Vector3(leftStick[0], 0.0f, -leftStick[1]) * (fly * moveMult));

        // gamepad rotate
        appendVector(rotate, Vector3(rightStick[0], rightStick[1], 0.0f) * (fly * rotateJoystickMult));

        // focus ends when it arrives or any input interrupts it
        if (_mode == Mode::FOCUS) {
            const bool interrupt = move.length() + rotate.length() > 0.0f;
            if (interrupt || _focusController.complete()) {
                setMode(Mode::ORBIT);
            }
        }

        _pose.copy(_controller->updatePose(_frame, dt));
        applyPose();
    }
}
