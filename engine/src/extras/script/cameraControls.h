// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 28.12.2025
//
// Script over the input
// framework in extras/input: an orbit, a fly and a focus controller, fed by a keyboard
// and mouse source, a multi-touch source (orbit), a dual-gesture source (fly) and a
// gamepad source. Left or middle button and the wheel switch to orbit, the right
// button or a movement key to fly; F-style focusing glides through the focus
// controller and hands back to orbit when it arrives or any input interrupts it.
//
// DEVIATION: the sources listen to the engine's input devices (the application's
// event loop feeds them), not to the canvas; there is no XR frame discard. The camera
// rotation is set as a WORLD rotation.
//
#pragma once

#include <memory>
#include <string>

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

    class CameraControls : public Script
    {
    public:
        SCRIPT_NAME("CameraControls")

        enum class Mode { ORBIT, FLY, FOCUS };

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
        void setInputBlocked(const bool blocked) { _desktopInput.setPointerBlocked(blocked); }

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

        struct State
        {
            Vector3 axis = Vector3(0.0f);
            float shift = 0.0f;
            float ctrl = 0.0f;
            float mouse[3] = {0.0f, 0.0f, 0.0f};
            float touches = 0.0f;
        } _state;

        bool _hasResetState = false;
        Vector3 _resetFocus = Vector3(0.0f);
        Vector3 _resetPosition = Vector3(0.0f);

        bool _autoFarClip = false;
        float _farClipScale = 10.0f;
        float _farClipMin = 1000.0f;
    };
}

// Register the script type globally (must be outside namespace)
REGISTER_SCRIPT(visutwin::canvas::CameraControls, "CameraControls")
