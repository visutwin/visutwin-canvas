// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "transformGizmo.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <SDL3/SDL_mouse.h>

#include "framework/components/render/renderComponent.h"
#include "framework/components/componentSystem.h"
#include "framework/engine.h"
#include "scene/materials/standardMaterial.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr float PICK_RADIUS_PX = 16.0f;

        float clamp01(const float v)
        {
            return std::clamp(v, 0.0f, 1.0f);
        }

        float snapValue(const float value, const float increment)
        {
            if (increment <= 0.0f) {
                return value;
            }
            return std::round(value / increment) * increment;
        }

        float pointSegmentDistanceSquared(
            const float px, const float py,
            const float ax, const float ay,
            const float bx, const float by)
        {
            const float abx = bx - ax;
            const float aby = by - ay;
            const float apx = px - ax;
            const float apy = py - ay;
            const float abLenSq = abx * abx + aby * aby;
            if (abLenSq <= 1e-6f) {
                const float dx = px - ax;
                const float dy = py - ay;
                return dx * dx + dy * dy;
            }

            const float t = clamp01((apx * abx + apy * aby) / abLenSq);
            const float cx = ax + abx * t;
            const float cy = ay + aby * t;
            const float dx = px - cx;
            const float dy = py - cy;
            return dx * dx + dy * dy;
        }
    }

    TransformGizmo::TransformGizmo(Engine* engine, CameraComponent* camera)
        : _engine(engine), _camera(camera)
    {
        // DEVIATION: this native port uses a lightweight gizmo entity hierarchy instead of
        // Upstream Gizmo/Shape classes from extras/gizmo.
        _root = new Entity();
        _root->setEngine(engine);
        if (_engine && _engine->root()) {
            _engine->root()->addChild(_root);
        }

        _handleX.axis = Axis::X;
        _handleX.baseColor = Color(1.0f, 0.0f, 0.0f, 1.0f);
        _handleX.entity = createHandleEntity("cone", _handleX.baseColor);
        _handleX.render = _handleX.entity ? _handleX.entity->findComponent<RenderComponent>() : nullptr;
        _handleX.material = _handleX.render ? static_cast<StandardMaterial*>(_handleX.render->material()) : nullptr;

        _handleY.axis = Axis::Y;
        _handleY.baseColor = Color(0.0f, 1.0f, 0.0f, 1.0f);
        _handleY.entity = createHandleEntity("cone", _handleY.baseColor);
        _handleY.render = _handleY.entity ? _handleY.entity->findComponent<RenderComponent>() : nullptr;
        _handleY.material = _handleY.render ? static_cast<StandardMaterial*>(_handleY.render->material()) : nullptr;

        _handleZ.axis = Axis::Z;
        _handleZ.baseColor = Color(0.0f, 0.35f, 1.0f, 1.0f);
        _handleZ.entity = createHandleEntity("cone", _handleZ.baseColor);
        _handleZ.render = _handleZ.entity ? _handleZ.entity->findComponent<RenderComponent>() : nullptr;
        _handleZ.material = _handleZ.render ? static_cast<StandardMaterial*>(_handleZ.render->material()) : nullptr;

        _handleCenter.axis = Axis::XYZ;
        _handleCenter.baseColor = Color(0.92f, 0.92f, 0.92f, 1.0f);
        _handleCenter.entity = createHandleEntity("sphere", _handleCenter.baseColor);
        _handleCenter.render = _handleCenter.entity ? _handleCenter.entity->findComponent<RenderComponent>() : nullptr;
        _handleCenter.material = _handleCenter.render ? static_cast<StandardMaterial*>(_handleCenter.render->material()) : nullptr;

        _shaftX.axis = Axis::X;
        _shaftX.baseColor = _handleX.baseColor;
        _shaftX.entity = createHandleEntity("cylinder", _shaftX.baseColor);
        _shaftX.render = _shaftX.entity ? _shaftX.entity->findComponent<RenderComponent>() : nullptr;
        _shaftX.material = _shaftX.render ? static_cast<StandardMaterial*>(_shaftX.render->material()) : nullptr;

        _shaftY.axis = Axis::Y;
        _shaftY.baseColor = _handleY.baseColor;
        _shaftY.entity = createHandleEntity("cylinder", _shaftY.baseColor);
        _shaftY.render = _shaftY.entity ? _shaftY.entity->findComponent<RenderComponent>() : nullptr;
        _shaftY.material = _shaftY.render ? static_cast<StandardMaterial*>(_shaftY.render->material()) : nullptr;

        _shaftZ.axis = Axis::Z;
        _shaftZ.baseColor = _handleZ.baseColor;
        _shaftZ.entity = createHandleEntity("cylinder", _shaftZ.baseColor);
        _shaftZ.render = _shaftZ.entity ? _shaftZ.entity->findComponent<RenderComponent>() : nullptr;
        _shaftZ.material = _shaftZ.render ? static_cast<StandardMaterial*>(_shaftZ.render->material()) : nullptr;

        setMode(Mode::Translate);
    }

    Entity* TransformGizmo::createHandleEntity(const char* primitiveType, const Color& color)
    {
        if (!_engine || !_root) {
            return nullptr;
        }

        auto* entity = new Entity();
        entity->setEngine(_engine);

        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(color);
        material->setEmissive(color);
        // Keep emissive modest to avoid tonemapping-driven white clipping.
        material->setEmissiveIntensity(1.0f);
        // Keep axis colors stable regardless of scene lights.
        material->setUseLighting(false);
        material->setUseSkybox(false);
        // Ensure both StandardMaterial and base Material uniform paths see the same color.
        material->setBaseColorFactor(color);
        material->setEmissiveFactor(color);
        _materials.push_back(material);

        auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
        if (render) {
            render->setType(primitiveType);
            render->setMaterial(material.get());
            // DEVIATION: render gizmo on world layer to avoid composition-specific UI sublayer behavior.
            render->setLayers({LAYERID_WORLD});
        }

        _root->addChild(entity);
        return entity;
    }

    void TransformGizmo::attach(Entity* target)
    {
        _target = target;
        _hoveredAxis = Axis::None;
        _activeAxis = Axis::None;
        _dragging = false;
        update();
    }

    void TransformGizmo::setMode(const Mode mode)
    {
        _mode = mode;

        const char* axisType = "cone";
        const char* centerType = "sphere";
        switch (_mode) {
            case Mode::Translate:
                axisType = "cone";
                centerType = "sphere";
                break;
            case Mode::Rotate:
                // DEVIATION: upstream rotate gizmo uses arc rings. This implementation currently visualizes
                // rotate handles as axis cylinders.
                axisType = "cylinder";
                centerType = "sphere";
                break;
            case Mode::Scale:
                // DEVIATION: upstream scale gizmo uses box-line + plane handles. This implementation uses
                // box endpoints and center box for scale interaction.
                axisType = "box";
                centerType = "box";
                break;
        }

        if (_handleX.render) _handleX.render->setType(axisType);
        if (_handleY.render) _handleY.render->setType(axisType);
        if (_handleZ.render) _handleZ.render->setType(axisType);
        if (_handleCenter.render) _handleCenter.render->setType(centerType);

        updateHandleTransforms();
        updateHandleColors();
    }

    bool TransformGizmo::handleEvent(const SDL_Event& event, const int windowWidth, const int windowHeight)
    {
        _windowWidth = std::max(1, windowWidth);
        _windowHeight = std::max(1, windowHeight);

        if (!_target) {
            return false;
        }

        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            const float mx = event.motion.x;
            const float my = event.motion.y;
            if (_dragging) {
                applyDrag(mx, my);
                return true;
            }

            _hoveredAxis = pickAxis(mx, my);
            updateHandleColors();
            return _hoveredAxis != Axis::None;
        }

        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
            const float mx = event.button.x;
            const float my = event.button.y;
            const Axis axis = pickAxis(mx, my);
            if (axis == Axis::None) {
                return false;
            }
            beginDrag(axis, mx, my);
            return true;
        }

        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            if (_dragging) {
                endDrag();
                return true;
            }
        }

        return false;
    }

    void TransformGizmo::update()
    {
        updateHandleTransforms();
        updateHandleColors();
    }

    void TransformGizmo::updateHandleTransforms()
    {
        if (!_target || !_root) {
            if (_root) {
                _root->setEnabled(false);
            }
            return;
        }

        _root->setEnabled(true);

        const Vector3 center = _target->position();
        _root->setPosition(center);

        const Vector3 ax = axisDirection(Axis::X);
        const Vector3 ay = axisDirection(Axis::Y);
        const Vector3 az = axisDirection(Axis::Z);

        _handleX.worldPosition = center + ax * _gizmoSize;
        _handleY.worldPosition = center + ay * _gizmoSize;
        _handleZ.worldPosition = center + az * _gizmoSize;
        _handleCenter.worldPosition = center;
        // Cone height in local Y is 0.22 units. The cone primitive's origin sits at its
        // geometric center, so the base is at -halfConeHeight and the tip at +halfConeHeight
        // in local space. To flush the cone base against the shaft tip the cone center must
        // be placed at (shaftLength + halfConeHeight) along the axis — which equals
        // (_gizmoSize - halfConeHeight) since _gizmoSize == shaftLength + fullConeHeight.
        constexpr float coneHeight = 0.22f;
        constexpr float halfConeHeight = coneHeight * 0.5f;
        const float shaftLength = std::max(0.05f, _gizmoSize - coneHeight);
        const float shaftRadius = (_mode == Mode::Scale) ? 0.03f : 0.02f;
        const bool showShafts = (_mode != Mode::Rotate);

        if (_handleX.entity) {
            _handleX.entity->setLocalEulerAngles(0.0f, 0.0f, -90.0f);
            if (_mode == Mode::Rotate) {
                _handleX.entity->setPosition(_handleX.worldPosition);
                _handleX.entity->setLocalScale(0.06f, _gizmoSize * 0.55f, 0.06f);
            } else if (_mode == Mode::Scale) {
                _handleX.entity->setPosition(_handleX.worldPosition);
                _handleX.entity->setLocalScale(0.16f, 0.16f, 0.16f);
            } else {
                // Place cone center at shaftLength + halfConeHeight so its base meets the shaft tip.
                _handleX.entity->setPosition(center + ax * (shaftLength + halfConeHeight));
                _handleX.entity->setLocalScale(0.08f, coneHeight, 0.08f);
            }
        }
        if (_shaftX.entity) {
            _shaftX.entity->setEnabled(showShafts);
            _shaftX.entity->setPosition(center + ax * (shaftLength * 0.5f));
            _shaftX.entity->setLocalEulerAngles(0.0f, 0.0f, -90.0f);
            _shaftX.entity->setLocalScale(shaftRadius, shaftLength, shaftRadius);
        }

        if (_handleY.entity) {
            _handleY.entity->setLocalEulerAngles(0.0f, 0.0f, 0.0f);
            if (_mode == Mode::Rotate) {
                _handleY.entity->setPosition(_handleY.worldPosition);
                _handleY.entity->setLocalScale(0.06f, _gizmoSize * 0.55f, 0.06f);
            } else if (_mode == Mode::Scale) {
                _handleY.entity->setPosition(_handleY.worldPosition);
                _handleY.entity->setLocalScale(0.16f, 0.16f, 0.16f);
            } else {
                _handleY.entity->setPosition(center + ay * (shaftLength + halfConeHeight));
                _handleY.entity->setLocalScale(0.08f, coneHeight, 0.08f);
            }
        }
        if (_shaftY.entity) {
            _shaftY.entity->setEnabled(showShafts);
            _shaftY.entity->setPosition(center + ay * (shaftLength * 0.5f));
            _shaftY.entity->setLocalEulerAngles(0.0f, 0.0f, 0.0f);
            _shaftY.entity->setLocalScale(shaftRadius, shaftLength, shaftRadius);
        }

        if (_handleZ.entity) {
            _handleZ.entity->setLocalEulerAngles(90.0f, 0.0f, 0.0f);
            if (_mode == Mode::Rotate) {
                _handleZ.entity->setPosition(_handleZ.worldPosition);
                _handleZ.entity->setLocalScale(0.06f, _gizmoSize * 0.55f, 0.06f);
            } else if (_mode == Mode::Scale) {
                _handleZ.entity->setPosition(_handleZ.worldPosition);
                _handleZ.entity->setLocalScale(0.16f, 0.16f, 0.16f);
            } else {
                _handleZ.entity->setPosition(center + az * (shaftLength + halfConeHeight));
                _handleZ.entity->setLocalScale(0.08f, coneHeight, 0.08f);
            }
        }
        if (_shaftZ.entity) {
            _shaftZ.entity->setEnabled(showShafts);
            _shaftZ.entity->setPosition(center + az * (shaftLength * 0.5f));
            _shaftZ.entity->setLocalEulerAngles(90.0f, 0.0f, 0.0f);
            _shaftZ.entity->setLocalScale(shaftRadius, shaftLength, shaftRadius);
        }

        if (_handleCenter.entity) {
            _handleCenter.entity->setPosition(_handleCenter.worldPosition);
            if (_mode == Mode::Scale) {
                _handleCenter.entity->setLocalScale(0.18f, 0.18f, 0.18f);
            } else {
                _handleCenter.entity->setLocalScale(0.12f, 0.12f, 0.12f);
            }
        }
    }

    void TransformGizmo::updateHandleColors()
    {
        auto applyColor = [&](Handle& handle) {
            if (!handle.material) {
                return;
            }

            Color color = handle.baseColor;
            if (_activeAxis == handle.axis) {
                color = Color(1.0f, 0.95f, 0.35f, 1.0f);
            } else if (_hoveredAxis == handle.axis) {
                color = Color(
                    std::min(handle.baseColor.r + 0.3f, 1.0f),
                    std::min(handle.baseColor.g + 0.3f, 1.0f),
                    std::min(handle.baseColor.b + 0.3f, 1.0f),
                    1.0f
                );
            }

            handle.material->setDiffuse(color);
            handle.material->setEmissive(color);
            handle.material->setBaseColorFactor(color);
            handle.material->setEmissiveFactor(color);
        };

        applyColor(_handleX);
        applyColor(_handleY);
        applyColor(_handleZ);
        applyColor(_shaftX);
        applyColor(_shaftY);
        applyColor(_shaftZ);
        applyColor(_handleCenter);
    }

    TransformGizmo::Axis TransformGizmo::pickAxis(const float mouseX, const float mouseY) const
    {
        if (!_target) {
            return Axis::None;
        }

        float cx = 0.0f;
        float cy = 0.0f;
        if (!worldToScreen(_handleCenter.worldPosition, cx, cy)) {
            return Axis::None;
        }

        const float centerDx = mouseX - cx;
        const float centerDy = mouseY - cy;
        if (centerDx * centerDx + centerDy * centerDy <= PICK_RADIUS_PX * PICK_RADIUS_PX) {
            return Axis::XYZ;
        }

        const std::array<Axis, 3> axes = {Axis::X, Axis::Y, Axis::Z};
        float bestDistance = std::numeric_limits<float>::max();
        Axis bestAxis = Axis::None;

        for (const Axis axis : axes) {
            const Vector3 start = _target->position();
            const Vector3 end = start + axisDirection(axis) * _gizmoSize;

            float sx = 0.0f;
            float sy = 0.0f;
            float ex = 0.0f;
            float ey = 0.0f;
            if (!worldToScreen(start, sx, sy) || !worldToScreen(end, ex, ey)) {
                continue;
            }

            const float d2 = pointSegmentDistanceSquared(mouseX, mouseY, sx, sy, ex, ey);
            if (d2 < bestDistance) {
                bestDistance = d2;
                bestAxis = axis;
            }
        }

        if (bestAxis == Axis::None || bestDistance > PICK_RADIUS_PX * PICK_RADIUS_PX) {
            return Axis::None;
        }

        return bestAxis;
    }

    bool TransformGizmo::worldToScreen(const Vector3& world, float& outX, float& outY) const
    {
        if (!_camera || !_camera->camera() || !_camera->entity()) {
            return false;
        }

        const Matrix4 view = _camera->entity()->worldTransform().inverse();
        const Matrix4 proj = _camera->camera()->projectionMatrix();
        const Vector3 viewPos = view.transformPoint(world);
        if (viewPos.getZ() >= 0.0f) {
            return false;
        }

        const Vector4 clip = proj * Vector4(viewPos.getX(), viewPos.getY(), viewPos.getZ(), 1.0f);
        if (std::abs(clip.getW()) < 1e-6f) {
            return false;
        }

        const float ndcX = clip.getX() / clip.getW();
        const float ndcY = clip.getY() / clip.getW();
        outX = (ndcX * 0.5f + 0.5f) * _windowWidth;
        outY = (1.0f - (ndcY * 0.5f + 0.5f)) * _windowHeight;
        return true;
    }

    Vector3 TransformGizmo::axisDirection(const Axis axis) const
    {
        switch (axis) {
            case Axis::X:
                return Vector3(1.0f, 0.0f, 0.0f);
            case Axis::Y:
                return Vector3(0.0f, 1.0f, 0.0f);
            case Axis::Z:
                return Vector3(0.0f, 0.0f, 1.0f);
            case Axis::XYZ:
            case Axis::None:
            default:
                return Vector3(0.0f, 0.0f, 0.0f);
        }
    }

    Vector3 TransformGizmo::cameraRight() const
    {
        if (!_camera || !_camera->entity()) {
            return Vector3(1.0f, 0.0f, 0.0f);
        }
        const auto& wt = _camera->entity()->worldTransform();
        const Vector4 c0 = wt.getColumn(0);
        return Vector3(c0.getX(), c0.getY(), c0.getZ()).normalized();
    }

    Vector3 TransformGizmo::cameraUp() const
    {
        if (!_camera || !_camera->entity()) {
            return Vector3(0.0f, 1.0f, 0.0f);
        }
        const auto& wt = _camera->entity()->worldTransform();
        const Vector4 c1 = wt.getColumn(1);
        return Vector3(c1.getX(), c1.getY(), c1.getZ()).normalized();
    }

    Vector3 TransformGizmo::cameraForward() const
    {
        if (!_camera || !_camera->entity()) {
            return Vector3(0.0f, 0.0f, -1.0f);
        }
        const auto& wt = _camera->entity()->worldTransform();
        const Vector4 c2 = wt.getColumn(2);
        return Vector3(c2.getX(), c2.getY(), c2.getZ()).normalized();
    }

    void TransformGizmo::beginDrag(const Axis axis, const float mouseX, const float mouseY)
    {
        if (!_target) {
            return;
        }

        _dragging = true;
        _activeAxis = axis;
        _hoveredAxis = axis;

        _dragStartMouseX = mouseX;
        _dragStartMouseY = mouseY;

        _targetStartPosition = _target->position();
        _targetStartRotation = _target->rotation();
        _targetStartScale = _target->localScale();

        SDL_CaptureMouse(true);
        updateHandleColors();
    }

    void TransformGizmo::applyDrag(const float mouseX, const float mouseY)
    {
        if (!_target || !_dragging || _activeAxis == Axis::None) {
            return;
        }

        const float dx = mouseX - _dragStartMouseX;
        const float dy = mouseY - _dragStartMouseY;

        if (_mode == Mode::Translate) {
            if (_activeAxis == Axis::XYZ) {
                const float upp = unitsPerPixelAtTarget();
                Vector3 delta = cameraRight() * (dx * upp) + cameraUp() * (-dy * upp);
                Vector3 pos = _targetStartPosition + delta;
                if (_snap) {
                    pos = Vector3(
                        snapValue(pos.getX(), _translateSnapIncrement),
                        snapValue(pos.getY(), _translateSnapIncrement),
                        snapValue(pos.getZ(), _translateSnapIncrement)
                    );
                }
                _target->setPosition(pos);
            } else {
                const Vector3 axis = axisDirection(_activeAxis);
                float sx0 = 0.0f;
                float sy0 = 0.0f;
                float sx1 = 0.0f;
                float sy1 = 0.0f;
                const Vector3 p0 = _targetStartPosition;
                const Vector3 p1 = p0 + axis * _gizmoSize;
                if (!worldToScreen(p0, sx0, sy0) || !worldToScreen(p1, sx1, sy1)) {
                    return;
                }

                const float ax = sx1 - sx0;
                const float ay = sy1 - sy0;
                const float len = std::sqrt(ax * ax + ay * ay);
                if (len < 1e-4f) {
                    return;
                }

                const float dirx = ax / len;
                const float diry = ay / len;
                const float projectedPixels = dx * dirx + dy * diry;
                const float pixelsPerWorld = len / _gizmoSize;
                float worldDelta = projectedPixels / std::max(pixelsPerWorld, 1e-4f);

                float along = axis.dot(_targetStartPosition + axis * worldDelta);
                if (_snap) {
                    along = snapValue(along, _translateSnapIncrement);
                }
                const Vector3 perp = _targetStartPosition - axis * axis.dot(_targetStartPosition);
                const Vector3 pos = perp + axis * along;
                _target->setPosition(pos);
            }
        } else if (_mode == Mode::Rotate) {
            Vector3 axis = (_activeAxis == Axis::XYZ) ? cameraForward() : axisDirection(_activeAxis);
            float angleDeg = dx * 0.35f;
            if (_snap) {
                angleDeg = snapValue(angleDeg, _rotateSnapIncrement);
            }
            const Quaternion q = Quaternion::fromAxisAngle(axis, angleDeg);
            _target->setRotation(q * _targetStartRotation);
        } else {
            float factor = 1.0f + dx * 0.01f;
            factor = std::max(factor, 0.05f);
            if (_snap) {
                factor = 1.0f + snapValue(factor - 1.0f, _scaleSnapIncrement);
            }

            Vector3 scale = _targetStartScale;
            if (_activeAxis == Axis::X) {
                scale = Vector3(std::max(0.05f, _targetStartScale.getX() * factor), scale.getY(), scale.getZ());
            } else if (_activeAxis == Axis::Y) {
                scale = Vector3(scale.getX(), std::max(0.05f, _targetStartScale.getY() * factor), scale.getZ());
            } else if (_activeAxis == Axis::Z) {
                scale = Vector3(scale.getX(), scale.getY(), std::max(0.05f, _targetStartScale.getZ() * factor));
            } else {
                scale = _targetStartScale * factor;
                scale = Vector3(
                    std::max(0.05f, scale.getX()),
                    std::max(0.05f, scale.getY()),
                    std::max(0.05f, scale.getZ())
                );
            }
            _target->setLocalScale(scale);
        }

        updateHandleTransforms();
    }

    void TransformGizmo::endDrag()
    {
        _dragging = false;
        _activeAxis = Axis::None;
        SDL_CaptureMouse(false);
        updateHandleColors();
    }

    float TransformGizmo::unitsPerPixelAtTarget() const
    {
        if (!_camera || !_camera->camera() || !_camera->entity() || !_target) {
            return 0.01f;
        }

        if (_camera->camera()->projection() == ProjectionType::Orthographic) {
            return (_camera->camera()->orthoHeight() * 2.0f) / _windowHeight;
        }

        const float fovRad = _camera->camera()->fov() * DEG_TO_RAD;
        const float distance = (_target->position() - _camera->entity()->position()).length();
        const float worldHeight = 2.0f * std::tan(fovRad * 0.5f) * std::max(distance, 0.01f);
        return worldHeight / _windowHeight;
    }
}
