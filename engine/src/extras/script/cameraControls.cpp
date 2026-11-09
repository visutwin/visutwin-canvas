// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 28.12.2025
//
#include "extras/script/cameraControls.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <spdlog/spdlog.h>

#include "core/math/quaternion.h"
#include "extras/input/sources/sourceEvents.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/input/inputConstants.h"
#include "platform/input/mouse.h"

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

        /// Left, middle and right as 0, 1 and 2, the order of the actions; -1 for any other.
        int mouseButtonIndex(const MouseButton button)
        {
            switch (button) {
            case MouseButton::Left: return 0;
            case MouseButton::Middle: return 1;
            case MouseButton::Right: return 2;
            default: return -1;
            }
        }
    }

    CameraControls::CameraControls()
    {
        // Orbit zooms down to 0.01 with no upper limit.
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

        // Inputs gathered while disabled are discarded.
        on("state", [this](bool) {
            _activeButton = -1;
            _pressedButton = -1;
            discardInputs();
        });
    }

    CameraControls::~CameraControls()
    {
        releasePointerHandles();
        _desktopInput.destroy();
        _orbitMobileInput.destroy();
        _flyMobileInput.destroy();
        _gamepadInput.destroy();
    }

    void CameraControls::releasePointerHandles()
    {
        releaseHandles(_pointerHandles);
    }

    void CameraControls::onMouseDown(const MouseEvent& event)
    {
        _pointer = Vector2(event.x, event.y);
        const int button = mouseButtonIndex(event.button);
        // A press the desktop source ignores (synthesized from a touch, or while the input is
        // blocked) presses no button here either.
        if (event.fromTouch || _inputBlocked || button < 0) {
            return;
        }

        // The movement since the last update belongs to the drag before this press. An update
        // applies one action, so the movement is dropped when that drag had another action,
        // rather than applied by the action of this button.
        const int owner = _pressedButton >= 0 ? _pressedButton : _activeButton;
        if (owner >= 0 && _buttonActions[owner] != _buttonActions[button]) {
            _desktopInput.deltas.at("mouse").read();
        }
        _pressedButton = button;
    }

    void CameraControls::setGroundPan(const bool enable)
    {
        _groundPan = enable;
        _orbitController.horizontalPan = enable;
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
        // Moving the camera now rather than on the next update keeps a frame
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

    Vector3 CameraControls::screenToGroundPan(const float x, const float y, const float dx, const float dy,
                                              const float height, const float yaw) const
    {
        const Camera* camera = _camera ? _camera->camera() : nullptr;
        if (!camera || (dx == 0.0f && dy == 0.0f)) {
            return Vector3(0.0f);
        }

        // The point of the plane under a canvas position: the ray from the near to the far clip
        // distance, or none when the plane is not in view there.
        const auto toPlane = [&](const float sx, const float sy, Vector3& out) {
            const Vector3 nearPoint = _camera->screenToWorld(sx, sy, camera->nearClip());
            const Vector3 farPoint = _camera->screenToWorld(sx, sy, camera->farClip());
            const float t = (height - nearPoint.getY()) / (farPoint.getY() - nearPoint.getY());
            if (!(t >= 0.0f && t <= 1.0f)) {
                return false;
            }
            out = Vector3::lerp(nearPoint, farPoint, t);
            return true;
        };
        Vector3 before;
        Vector3 after;
        if (!toPlane(x - dx, y - dy, before) || !toPlane(x, y, after)) {
            return Vector3(0.0f);
        }

        // The view moves against the cursor, measured along the right and forward directions
        // turned by the yaw alone: the directions the orbit controller moves the focus point
        // along. The directions of the camera itself would flip once it looks past straight down.
        const Vector3 shift = before - after;
        const Quaternion yawRotation = Quaternion::fromEulerAngles(0.0f, yaw, 0.0f);
        const Vector3 right = yawRotation * Vector3(1.0f, 0.0f, 0.0f);
        const Vector3 forward = yawRotation * Vector3(0.0f, 0.0f, -1.0f);
        return Vector3(shift.dot(right), shift.dot(forward), 0.0f);
    }

    void CameraControls::appendMouseMove(const MouseAction action, const float dx, const float dy, const float orbit)
    {
        // A button ignored by the camera leaves its mouse movement to the application. Panning
        // follows the pan action, or shift with any other.
        const float follow = action != MouseAction::NONE ? 1.0f : 0.0f;
        const float pan = (action == MouseAction::PAN || (_state.shift != 0.0f && follow != 0.0f)) ? 1.0f : 0.0f;

        const Vector3 panMove = _groundPan
            ? screenToGroundPan(_pointer.x, _pointer.y, dx, dy, _pose.getFocus().getY(), _pose.angles.getY())
            : screenToWorld(dx, dy, _pose.distance);
        appendVector(_frame.delta("move"), panMove * (orbit * pan * (_enablePan ? 1.0f : 0.0f)));

        const float rotate = follow * (1.0f - orbit * pan) * _rotateSpeed;
        appendVector(_frame.delta("rotate"), Vector3(dx * rotate, dy * rotate, 0.0f));
    }

    void CameraControls::update(float dt)
    {
        if (!ensureSetup()) {
            return;
        }
        if (!_sourcesAttached) {
            if (Engine* engine = entity()->engine()) {
                // The cursor position is tracked for the ground pan, which holds the ground
                // under it, and the pressed button is taken from the press itself. Subscribed
                // before the desktop source, so a press is seen here first.
                if (Mouse* mouse = engine->mouse()) {
                    _pointerHandles.push_back(mouse->on("mousedown", [this](const MouseEvent& e) { onMouseDown(e); }));
                    _pointerHandles.push_back(mouse->on("mousemove", [this](const MouseEvent& e) {
                        _pointer = Vector2(e.x, e.y);
                    }));
                }
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

        // A pressed button starts the mode of its action, and the mouse movement follows the
        // action of the last button pressed.
        const int pressedButton = _pressedButton;
        _pressedButton = -1;
        if (pressedButton >= 0) {
            _activeButton = pressedButton;
        }
        const MouseAction pressed = pressedButton >= 0 ? _buttonActions[pressedButton] : MouseAction::NONE;
        const MouseAction action = _activeButton >= 0 ? _buttonActions[_activeButton] : MouseAction::NONE;

        if (wheel[0] != 0.0f || pressed == MouseAction::ORBIT || pressed == MouseAction::PAN) {
            setMode(Mode::ORBIT);
        } else if (pressed == MouseAction::LOOK || _state.axis.length() > 0.0f) {
            setMode(Mode::FLY);
        }

        const float orbit = _mode == Mode::ORBIT ? 1.0f : 0.0f;
        const float fly = _mode == Mode::FLY ? 1.0f : 0.0f;
        const float twoFingers = _state.touches > 1.0f ? 1.0f : 0.0f;
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
        v += Vector3(0.0f, 0.0f, wheel[0]) * (orbit * zoomDeltaMult);
        appendVector(move, v);

        // desktop mouse
        appendMouseMove(action, mouse[0], mouse[1], orbit);

        // the movement of the button has been applied, until it is released
        if (_activeButton >= 0 && _state.mouse[_activeButton] <= 0.0f) {
            _activeButton = -1;
        }

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
