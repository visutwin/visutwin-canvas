// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 28.12.2025
//
#pragma once

#include <extras/input/pose.h>
#include <framework/components/camera/cameraComponent.h>

#include "core/math/vector3.h"
#include "framework/script/script.h"
#include "framework/script/scriptRegistry.h"
#include "extras/input/input.h"

namespace visutwin::canvas
{
    /**
     * CameraControls script provides comprehensive camera control functionality for both
     * desktop and mobile platforms. It supports multiple camera modes:
     *
     * - Orbit mode: Rotate around a focus point
     * - Fly mode: Free flying camera movement
     * - Focus mode: Smooth transition to look at a specific point
     *
     * The script handles input from:
     * - Keyboard and mouse (desktop)
     * - Touch gestures (mobile)
     * - Virtual joysticks (mobile)
     * - Gamepad controllers
     *
     * @category Script
     */
    class CameraControls: public Script
    {
    public:
        SCRIPT_NAME("CameraControls")

        void initialize() override;

        // Camera mode enumeration
        enum class Mode {
            ORBIT,  // Orbit around a focus point
            FLY,    // Free flying camera
            FOCUS   // Focusing on a point
        };

        // Called every frame to update the camera controls
        void update(float dt) override;

        void setFocusPoint(const Vector3& point);

        void setEnableFly(bool enable);

        void setMoveSpeed(float speed) { _moveSpeed = speed; }

        void setMoveFastSpeed(float speed) { _moveFastSpeed = speed; }

        void setMoveSlowSpeed(float speed) { _moveSlowSpeed = speed; }

        // set pitch (elevation) angle limits in degrees.
        // x = min pitch, y = max pitch. Default (-90, 90) prevents looking from underground.
        void setPitchRange(const Vector2& range) { _pitchRange = range; }

        void setOrbitDistance(float distance);

        [[nodiscard]] float orbitDistance() const { return _orbitDistance; }

        void addZoomInput(float delta) { _zoomImpulse += delta; }

        /// When enabled, CameraControls automatically keeps the camera's far
        /// clip plane at `max(orbitDistance * farClipScale, farClipMin)` every
        /// frame. This prevents geometry from being culled when zooming out.
        void setAutoFarClip(bool enable, float scale = 10.0f, float minimum = 1000.0f)
        {
            _autoFarClip = enable;
            _farClipScale = scale;
            _farClipMin = minimum;
        }

        void focus(const Vector3& point, float distance);

        void storeResetState();

        void reset();

        /// Block mouse/keyboard input processing (e.g. when UI overlay captures input).
        void setInputBlocked(bool blocked) { _inputBlocked = blocked; }

    private:
        void setMode(Mode mode);

        Mode _mode = Mode::ORBIT; // Current mode

        bool _enableOrbit = true;
        bool _enableFly = false;
        bool _inputBlocked = false;

        Vector2 _pitchRange = Vector2(-90.0f, 90.0f);
        Vector2 _yawRange = Vector2(0.0f, 0.0f);
        Vector2 _zoomRange = Vector2(0.0f, 0.0f);

        CameraComponent* _camera = nullptr;

        InputController* _controller = nullptr;

        float _startZoomDist = 0.0f;

        Pose _pose;

        Vector3 _focusPoint{0.0f, 0.0f, 0.0f};
        bool _hasFocusPoint = false;
        float _orbitDistance = 0.0f;
        float _pitch = 0.0f;
        float _yaw = 0.0f;
        bool _mouseOrbitActive = false;
        float _prevMouseX = 0.0f;
        float _prevMouseY = 0.0f;
        float _zoomImpulse = 0.0f;
        bool _hasResetState = false;
        Vector3 _resetFocusPoint{0.0f, 0.0f, 0.0f};
        float _resetOrbitDistance = 0.0f;
        float _resetPitch = 0.0f;
        float _resetYaw = 0.0f;

        // The fly move speed relative to the scene size
        float _moveSpeed = 10.0f;

        // The fast fly move speed relative to the scene size
        float _moveFastSpeed = 20.0f;

        // The slow fly move speed relative to the scene size
        float _moveSlowSpeed = 5.0f;

        // Auto far-clip tracking
        bool _autoFarClip = false;
        float _farClipScale = 10.0f;
        float _farClipMin = 1000.0f;
    };
}

// Register the script type globally (must be outside namespace)
REGISTER_SCRIPT(visutwin::canvas::CameraControls, "CameraControls")
