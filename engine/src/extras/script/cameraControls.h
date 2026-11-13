// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 28.12.2025
//
// Script over the input
// framework in extras/input: an orbit, a fly and a focus controller, fed by a keyboard
// and mouse source, a multi-touch source (orbit), a dual-gesture source (fly) and a
// gamepad source. Each mouse button has an action (orbit, pan, look or none; by
// default left orbits, middle pans, right looks): a button that orbits or pans and the
// wheel switch to orbit, a button that looks or a movement key to fly. F-style focusing
// glides through the focus controller and hands back to orbit when it arrives or any
// input interrupts it. With ground pan, panning moves across the horizontal plane at the
// focus point's height, holding the point of the ground under the cursor.
//
// DEVIATION: the sources listen to the engine's input devices (the application's
// event loop feeds them), not to the canvas; there is no XR frame discard. The camera
// rotation is set as a WORLD rotation. The cursor position the ground pan needs and the
// button a press names come from the engine's Mouse events, not canvas pointer events.
//
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "extras/input/controllers/flyController.h"
#include "extras/input/controllers/focusController.h"
#include "extras/input/controllers/orbitController.h"
#include "extras/input/input.h"
#include "extras/input/pose.h"
#include "extras/input/sources/dualGestureSource.h"
#include "extras/input/sources/gamepadSource.h"
#include "extras/input/sources/keyboardMouseSource.h"
#include "extras/input/sources/multiTouchSource.h"
#include "framework/script/script.h"
#include "framework/script/scriptRegistry.h"

namespace visutwin::canvas
{
    class CameraComponent;
    struct MouseEvent;

    class CameraControls : public Script
    {
    public:
        SCRIPT_NAME("CameraControls")

        enum class Mode { ORBIT, FLY, FOCUS };

        /// What a mouse button does.
        /// - ORBIT: turns the view around the focus point, or pans it while shift is held.
        /// - PAN: pans the view (see setGroundPan).
        /// - LOOK: turns the view in place, as when flying with the keys.
        /// - NONE: ignores the button and the drag that follows, leaving them to the application.
        ///
        /// DEVIATION: an enum where upstream takes strings, so there is no invalid action to
        /// warn about.
        enum class MouseAction { ORBIT, PAN, LOOK, NONE };

        CameraControls();
        ~CameraControls() override;

        void initialize() override;
        void update(float dt) override;

        [[nodiscard]] Mode mode() const { return _mode; }

        void setEnableFly(bool enable);
        [[nodiscard]] bool enableFly() const { return _enableFly; }
        void setEnableOrbit(bool enable);
        [[nodiscard]] bool enableOrbit() const { return _enableOrbit; }
        void setEnablePan(const bool enable) { _enablePan = enable; }
        [[nodiscard]] bool enablePan() const { return _enablePan; }

        void setFocusDamping(const float damping) { _focusController.focusDamping = damping; }
        [[nodiscard]] float focusDamping() const { return _focusController.focusDamping; }
        void setMoveDamping(const float damping) { _flyController.moveDamping = damping; }
        [[nodiscard]] float moveDamping() const { return _flyController.moveDamping; }

        void setRotateDamping(float damping);
        [[nodiscard]] float rotateDamping() const { return _orbitController.rotateDamping; }
        void setZoomDamping(const float damping) { _orbitController.zoomDamping = damping; }
        [[nodiscard]] float zoomDamping() const { return _orbitController.zoomDamping; }

        /// Look at `point` from where the camera is now, without smoothing.
        void setFocusPoint(const Vector3& point);
        [[nodiscard]] Vector3 focusPoint() const { return _pose.getFocus(); }

        void setMoveSpeed(const float speed) { _moveSpeed = speed; }
        void setMoveFastSpeed(const float speed) { _moveFastSpeed = speed; }
        void setMoveSlowSpeed(const float speed) { _moveSlowSpeed = speed; }
        void setRotateSpeed(const float speed) { _rotateSpeed = speed; }
        void setRotateJoystickSens(const float sens) { _rotateJoystickSens = sens; }
        void setZoomSpeed(const float speed) { _zoomSpeed = speed; }
        void setZoomPinchSens(const float sens) { _zoomPinchSens = sens; }

        /// x = closest, y = farthest orbit distance; y <= x means no upper limit.
        void setZoomRange(const Vector2& range);
        [[nodiscard]] const Vector2& zoomRange() const { return _zoomRange; }
        /// Pitch and yaw limits in degrees, each clamped to [-360, 360].
        void setPitchRange(const Vector2& range);
        [[nodiscard]] const Vector2& pitchRange() const { return _pitchRange; }
        void setYawRange(const Vector2& range);
        [[nodiscard]] const Vector2& yawRange() const { return _yawRange; }

        /// The engine event the fly joysticks fire, with ":left" / ":right" appended.
        void setJoystickEventName(const std::string& name) { _joystickEventName = name; }
        /// "joystick-touch" (default), "touch-joystick", "joystick-joystick" or "touch-touch".
        void setMobileInputLayout(const std::string& layout);
        [[nodiscard]] const std::string& mobileInputLayout() const { return _flyMobileInput.layout(); }
        void setGamepadDeadZone(const Vector2& zone) { _gamepadDeadZone = zone; }

        /// The action of the left mouse button; ORBIT by default.
        void setLeftButton(const MouseAction action) { _buttonActions[0] = action; }
        [[nodiscard]] MouseAction leftButton() const { return _buttonActions[0]; }
        /// The action of the middle mouse button; PAN by default.
        void setMiddleButton(const MouseAction action) { _buttonActions[1] = action; }
        [[nodiscard]] MouseAction middleButton() const { return _buttonActions[1]; }
        /// The action of the right mouse button; LOOK by default.
        void setRightButton(const MouseAction action) { _buttonActions[2] = action; }
        [[nodiscard]] MouseAction rightButton() const { return _buttonActions[2]; }

        /// Whether panning moves across the ground instead of the plane of the view (off by
        /// default). A mouse drag then holds the point of the ground under the cursor, as when
        /// dragging a map, and the view keeps its height. The ground is the horizontal plane at
        /// the height of the focus point. Panning with two touches moves across the ground too.
        void setGroundPan(bool enable);
        [[nodiscard]] bool groundPan() const { return _groundPan; }

        /// Glide to orbit `point`, keeping the view direction; at the current distance,
        /// or at the one setFocusPoint started from when resetZoom.
        void focus(const Vector3& point, bool resetZoom = false);
        /// Glide to look at `point` from where the camera is (or, with resetZoom, from
        /// the distance setFocusPoint started from).
        void look(const Vector3& point, bool resetZoom = false);
        /// Glide to look at `focus` from `position`.
        void reset(const Vector3& focus, const Vector3& position);

        // --- this port's conveniences ---------------------------------------------

        /// focus() at an explicit distance.
        void focus(const Vector3& point, float distance);

        /// Move the camera to `distance` from the focus point along its view, at once.
        void setOrbitDistance(float distance);
        [[nodiscard]] float orbitDistance() const { return _pose.distance; }

        /// Remember the current view for reset().
        void storeResetState();
        /// Glide back to the view storeResetState() remembered.
        void reset();

        /// Ignore presses and the wheel (e.g. while a HUD has the pointer).
        void setInputBlocked(const bool blocked)
        {
            _inputBlocked = blocked;
            _desktopInput.setPointerBlocked(blocked);
        }

        /// Keep the camera's far clip at least max(orbitDistance * scale, minimum).
        void setAutoFarClip(bool enable, float scale = 10.0f, float minimum = 1000.0f)
        {
            _autoFarClip = enable;
            _farClipScale = scale;
            _farClipMin = minimum;
        }

    private:
        bool ensureSetup();
        void setMode(Mode mode);
        void applyPose();
        [[nodiscard]] Vector3 cameraForward() const;
        [[nodiscard]] Vector3 screenToWorld(float dx, float dy, float dz) const;
        [[nodiscard]] Vector3 screenToGroundPan(float x, float y, float dx, float dy, float height, float yaw) const;
        void appendMouseMove(MouseAction action, float dx, float dy, float orbit);
        void onMouseDown(const MouseEvent& event);
        void releasePointerHandles();
        void discardInputs();

        CameraComponent* _camera = nullptr;
        bool _ready = false;
        bool _sourcesAttached = false;

        bool _enableOrbit = true;
        bool _enableFly = true;
        bool _enablePan = true;
        float _startZoomDist = 0.0f;
        Vector2 _pitchRange = Vector2(-360.0f, 360.0f);
        Vector2 _yawRange = Vector2(-360.0f, 360.0f);
        Vector2 _zoomRange = Vector2(0.01f, 0.0f);

        float _moveSpeed = 10.0f;
        float _moveFastSpeed = 20.0f;
        float _moveSlowSpeed = 5.0f;
        float _rotateSpeed = 0.2f;
        float _rotateJoystickSens = 2.0f;
        float _zoomSpeed = 0.001f;
        float _zoomPinchSens = 5.0f;
        std::string _joystickEventName = "joystick";
        Vector2 _gamepadDeadZone = Vector2(0.3f, 0.6f);

        KeyboardMouseSource _desktopInput;
        MultiTouchSource _orbitMobileInput;
        DualGestureSource _flyMobileInput;
        GamepadSource _gamepadInput;

        FlyController _flyController;
        OrbitController _orbitController;
        FocusController _focusController;
        InputController* _controller = nullptr;

        Pose _pose;
        Mode _mode = Mode::ORBIT;
        bool _hasMode = false;

        InputFrame _frame{{{"move", 3}, {"rotate", 3}}};

        // The action of each mouse button: left, middle, right.
        std::array<MouseAction, 3> _buttonActions{MouseAction::ORBIT, MouseAction::PAN, MouseAction::LOOK};
        // The button the mouse movement follows: the last one pressed, until the update
        // after its release, or -1.
        int _activeButton = -1;
        // The button pressed since the last update, or -1. Taken from the press itself, as
        // the button deltas of a press and a release between two updates cancel out while
        // the movement of the drag in between is still to be applied.
        int _pressedButton = -1;
        bool _groundPan = false;
        bool _inputBlocked = false;
        // The cursor position on the canvas, in points.
        Vector2 _pointer = Vector2(0.0f, 0.0f);
        std::vector<EventHandlePtr> _pointerHandles;

        struct State
        {
            Vector3 axis = Vector3(0.0f);
            float shift = 0.0f;
            float ctrl = 0.0f;
            float mouse[3] = {0.0f, 0.0f, 0.0f};
            float touches = 0.0f;
        } _state;

        bool _hasResetState = false;

        bool _storeResetOnSetup = false;
        Vector3 _resetFocus = Vector3(0.0f);
        Vector3 _resetPosition = Vector3(0.0f);

        bool _autoFarClip = false;
        float _farClipScale = 10.0f;
        float _farClipMin = 1000.0f;
    };
}

// Register the script type globally (must be outside namespace)
REGISTER_SCRIPT(visutwin::canvas::CameraControls, "CameraControls")
