// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

#include <SDL3/SDL_events.h>

#include "core/math/color.h"
#include "core/math/quaternion.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/entity.h"

namespace visutwin::canvas
{
    class Engine;
    class RenderComponent;
    class StandardMaterial;

    class TransformGizmo
    {
    public:
        enum class Mode
        {
            Translate,
            Rotate,
            Scale
        };

        enum class Axis
        {
            None,
            X,
            Y,
            Z,
            XYZ
        };

        TransformGizmo(Engine* engine, CameraComponent* camera);
        ~TransformGizmo() = default;

        void attach(Entity* target);

        void setMode(Mode mode);
        Mode mode() const { return _mode; }

        void setSnap(bool enabled) { _snap = enabled; }
        bool snap() const { return _snap; }

        void setTranslateSnapIncrement(float value) { _translateSnapIncrement = std::max(0.001f, value); }
        void setRotateSnapIncrement(float value) { _rotateSnapIncrement = std::max(0.1f, value); }
        void setScaleSnapIncrement(float value) { _scaleSnapIncrement = std::max(0.001f, value); }

        bool handleEvent(const SDL_Event& event, int windowWidth, int windowHeight);
        void update();

    private:
        struct Handle
        {
            Axis axis = Axis::None;
            Entity* entity = nullptr;
            RenderComponent* render = nullptr;
            StandardMaterial* material = nullptr;
            Color baseColor = Color(1.0f, 1.0f, 1.0f, 1.0f);
            Vector3 worldPosition = Vector3(0.0f, 0.0f, 0.0f);
        };

        Entity* createHandleEntity(const char* primitiveType, const Color& color);
        void updateHandleTransforms();
        void updateHandleColors();

        Axis pickAxis(float mouseX, float mouseY) const;
        bool worldToScreen(const Vector3& world, float& outX, float& outY) const;
        Vector3 axisDirection(Axis axis) const;
        Vector3 cameraRight() const;
        Vector3 cameraUp() const;
        Vector3 cameraForward() const;

        void beginDrag(Axis axis, float mouseX, float mouseY);
        void applyDrag(float mouseX, float mouseY);
        void endDrag();

        float unitsPerPixelAtTarget() const;

        Engine* _engine = nullptr;
        CameraComponent* _camera = nullptr;
        Entity* _target = nullptr;

        Entity* _root = nullptr;
        Handle _handleX;
        Handle _handleY;
        Handle _handleZ;
        Handle _handleCenter;
        Handle _shaftX;
        Handle _shaftY;
        Handle _shaftZ;

        std::vector<std::shared_ptr<StandardMaterial>> _materials;

        Mode _mode = Mode::Translate;
        Axis _hoveredAxis = Axis::None;
        Axis _activeAxis = Axis::None;
        bool _dragging = false;

        float _windowWidth = 1.0f;
        float _windowHeight = 1.0f;

        float _dragStartMouseX = 0.0f;
        float _dragStartMouseY = 0.0f;

        Vector3 _targetStartPosition = Vector3(0.0f, 0.0f, 0.0f);
        Quaternion _targetStartRotation;
        Vector3 _targetStartScale = Vector3(1.0f, 1.0f, 1.0f);

        float _gizmoSize = 1.8f;

        bool _snap = false;
        float _translateSnapIncrement = 0.5f;
        float _rotateSnapIncrement = 15.0f;
        float _scaleSnapIncrement = 0.1f;
    };
}
