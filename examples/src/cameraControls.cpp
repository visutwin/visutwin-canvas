// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 28.12.2025
//
#include "../cameraControls.h"
#include <algorithm>
#include <cmath>
#include <SDL3/SDL.h>
#include <framework/entity.h>

namespace visutwin::canvas
{
    namespace
    {
        constexpr float MIN_ORBIT_DISTANCE = 0.1f;
        constexpr float MOUSE_SENSITIVITY = 0.2f;
        constexpr float PAN_SENSITIVITY = 0.0025f;

        Vector3 computeForwardFromEuler(const float pitchDeg, const float yawDeg)
        {
            const float pitch = pitchDeg * DEG_TO_RAD;
            const float yaw = yawDeg * DEG_TO_RAD;
            const float cp = std::cos(pitch);
            return Vector3(
                -std::sin(yaw) * cp,
                std::sin(pitch),
                -std::cos(yaw) * cp
            );
        }
    }

    void CameraControls::initialize()
    {
        auto* owner = entity();
        if (!owner) {
            return;
        }

        auto cameraComponents = owner->findComponents<CameraComponent>();
        if (!cameraComponents.empty()) {
            _camera = cameraComponents.front();
        }
    }

    void CameraControls::setFocusPoint(const Vector3& point)
    {
        if (!_camera || !_camera->entity()) {
            return;
        }

        _focusPoint = point;
        _hasFocusPoint = true;

        const auto position = _camera->entity()->position();
        _orbitDistance = std::max(position.distance(point), MIN_ORBIT_DISTANCE);
        _startZoomDist = _orbitDistance;

        const Vector3 dir = (point - position).normalized();
        const float dx = dir.getX();
        const float dy = dir.getY();
        const float dz = dir.getZ();
        _pitch = std::asin(std::clamp(dy, -1.0f, 1.0f)) * RAD_TO_DEG;
        _yaw = std::atan2(-dx, -dz) * RAD_TO_DEG;

        _camera->entity()->setLocalEulerAngles(_pitch, _yaw, 0.0f);
    }

    void CameraControls::setEnableFly(bool enable)
    {
        _enableFly = enable;

        if (!_enableFly && _mode == Mode::FLY) {
            setMode(Mode::ORBIT);
        }
    }

    void CameraControls::setMode(Mode mode)
    {
        if (mode == Mode::FLY && !_enableFly) {
            _mode = Mode::ORBIT;
            return;
        }
        _mode = mode;
    }

    void CameraControls::setOrbitDistance(const float distance)
    {
        _orbitDistance = std::max(distance, MIN_ORBIT_DISTANCE);
    }

    void CameraControls::focus(const Vector3& point, const float distance)
    {
        _focusPoint = point;
        _hasFocusPoint = true;
        _orbitDistance = std::max(distance, MIN_ORBIT_DISTANCE);
    }

    void CameraControls::storeResetState()
    {
        if (!_hasFocusPoint) {
            return;
        }
        _resetFocusPoint = _focusPoint;
        _resetOrbitDistance = _orbitDistance;
        _resetPitch = _pitch;
        _resetYaw = _yaw;
        _hasResetState = true;
    }

    void CameraControls::reset()
    {
        if (!_hasResetState) {
            return;
        }
        _focusPoint = _resetFocusPoint;
        _orbitDistance = _resetOrbitDistance;
        _pitch = _resetPitch;
        _yaw = _resetYaw;
        _hasFocusPoint = true;
    }

    void CameraControls::update(float dt)
    {
        if (!_camera || !_camera->entity()) {
            return;
        }

        if (_mode == Mode::ORBIT && _hasFocusPoint) {
            float mouseX = 0.0f;
            float mouseY = 0.0f;
            const uint32_t rawButtons = SDL_GetMouseState(&mouseX, &mouseY);
            const uint32_t mouseButtons = _inputBlocked ? 0u : rawButtons;
            const bool orbitMouseDown = (mouseButtons & SDL_BUTTON_LMASK) != 0u || (mouseButtons & SDL_BUTTON_RMASK) != 0u;
            const bool middleMouseDown = (mouseButtons & SDL_BUTTON_MMASK) != 0u;
            const bool* keys = SDL_GetKeyboardState(nullptr);
            const bool shiftDown = keys && (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]);
            const bool panMouseDown = middleMouseDown || (shiftDown && orbitMouseDown);

            if (panMouseDown) {
                if (_mouseOrbitActive) {
                    const float dx = mouseX - _prevMouseX;
                    const float dy = mouseY - _prevMouseY;
                    const Vector3 forward = computeForwardFromEuler(_pitch, _yaw);
                    Vector3 right = forward.cross(Vector3::UNIT_Y);
                    if (right.length() < 1e-5f) {
                        right = Vector3::UNIT_X;
                    } else {
                        right = right.normalized();
                    }
                    Vector3 up = right.cross(forward);
                    if (up.length() < 1e-5f) {
                        up = Vector3::UNIT_Y;
                    } else {
                        up = up.normalized();
                    }
                    const float panScale = std::max(_orbitDistance, 1.0f) * PAN_SENSITIVITY;
                    _focusPoint -= right * (dx * panScale);
                    _focusPoint += up * (dy * panScale);
                }
                _mouseOrbitActive = true;
            } else if (orbitMouseDown) {
                if (_mouseOrbitActive) {
                    const float dx = mouseX - _prevMouseX;
                    const float dy = mouseY - _prevMouseY;
                    _yaw -= dx * MOUSE_SENSITIVITY;
                    _pitch -= dy * MOUSE_SENSITIVITY;
                    _pitch = std::clamp(_pitch, _pitchRange.x, _pitchRange.y);
                }
                _mouseOrbitActive = true;
            } else {
                _mouseOrbitActive = false;
            }
            _prevMouseX = mouseX;
            _prevMouseY = mouseY;

            if (keys) {
                const bool fast = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
                const bool slow = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];

                float zoomSpeed = _moveSpeed;
                if (fast) {
                    zoomSpeed = _moveFastSpeed;
                } else if (slow) {
                    zoomSpeed = _moveSlowSpeed;
                }
                // Scale zoom speed by current distance for responsive orbit zoom.
                const float distanceScale = std::max(_orbitDistance * 0.25f, 1.0f);
                zoomSpeed = std::max(zoomSpeed * distanceScale, 0.1f);

                if (keys[SDL_SCANCODE_W]) {
                    _orbitDistance -= zoomSpeed * dt;
                }
                if (keys[SDL_SCANCODE_S]) {
                    _orbitDistance += zoomSpeed * dt;
                }

                const float rotateSpeed = 70.0f * dt;
                if (keys[SDL_SCANCODE_A]) {
                    _yaw += rotateSpeed;
                }
                if (keys[SDL_SCANCODE_D]) {
                    _yaw -= rotateSpeed;
                }
                if (keys[SDL_SCANCODE_Q]) {
                    _pitch += rotateSpeed;
                }
                if (keys[SDL_SCANCODE_E]) {
                    _pitch -= rotateSpeed;
                }
                _pitch = std::clamp(_pitch, _pitchRange.x, _pitchRange.y);
            }

            if (std::abs(_zoomImpulse) > 0.0f) {
                const float wheelZoomStep = std::max(_orbitDistance * 0.04f, 0.005f);
                _orbitDistance -= _zoomImpulse * wheelZoomStep;
                _zoomImpulse = 0.0f;
            }

            _orbitDistance = std::max(_orbitDistance, MIN_ORBIT_DISTANCE);

            const Vector3 forward = computeForwardFromEuler(_pitch, _yaw);
            const Vector3 position = _focusPoint - forward * _orbitDistance;
            _camera->entity()->setPosition(position);
            _camera->entity()->setLocalEulerAngles(_pitch, _yaw, 0.0f);

            // Keep the far clip plane in sync with the orbit distance so that
            // geometry is never culled when zooming out.
            if (_autoFarClip) {
                const float desiredFar = std::max(_orbitDistance * _farClipScale, _farClipMin);
                if (_camera->camera() && _camera->camera()->farClip() < desiredFar) {
                    _camera->camera()->setFarClip(desiredFar);
                }
            }
        }
    }
}
