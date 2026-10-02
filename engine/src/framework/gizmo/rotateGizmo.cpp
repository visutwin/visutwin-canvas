// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "rotateGizmo.h"

#include <cmath>

#include "core/math/defines.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/entity.h"
#include "framework/gizmo/meshLine.h"
#include "framework/gizmo/shape/shapes.h"
#include "scene/camera.h"
#include "scene/layer.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr float RING_FACING_EPSILON = 1e-4f;
        constexpr float UPDATE_EPSILON = 1e-6f;

        float sign(const float v)
        {
            return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f);
        }

        bool equalsApprox(const Vector3& a, const Vector3& b, const float epsilon)
        {
            return std::abs(a.getX() - b.getX()) < epsilon && std::abs(a.getY() - b.getY()) < epsilon &&
                std::abs(a.getZ() - b.getZ()) < epsilon;
        }

        /// Upstream GraphNode.setEulerAngles / setRotation in WORLD space for a child of `parent`.
        void setWorldRotation(Entity* entity, Entity* parent, const Quaternion& rotation)
        {
            entity->setLocalRotation(parent ? parent->rotation().invert() * rotation : rotation);
        }
    }

    RotateGizmo::RotateGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer)
        : TransformGizmo(camera, std::move(layer), "gizmo:rotate")
    {
        snapIncrement = 5.0f;

        _arcs[2] = addShape<ArcShape>(GizmoAxis::Z, _engine, shapeArgs(GizmoAxis::Z, Vector3(90.0f, 0.0f, 90.0f)),
            ArcShapeArgs{.sectorAngle = 180.0f});
        _arcs[0] = addShape<ArcShape>(GizmoAxis::X, _engine, shapeArgs(GizmoAxis::X, Vector3(0.0f, 0.0f, -90.0f)),
            ArcShapeArgs{.sectorAngle = 180.0f});
        _arcs[1] = addShape<ArcShape>(GizmoAxis::Y, _engine, shapeArgs(GizmoAxis::Y, Vector3(0.0f, 0.0f, 0.0f)),
            ArcShapeArgs{.sectorAngle = 180.0f});
        _face = addShape<ArcShape>(GizmoAxis::F, _engine, shapeArgs(GizmoAxis::F), ArcShapeArgs{.ringRadius = 0.55f});
        _center = addShape<SphereShape>(GizmoAxis::XYZ, _engine, shapeArgs(GizmoAxis::XYZ), 0.5f);

        GizmoThemePartial theme;
        theme.shapeBase.xyz = Color(0.0f, 0.0f, 0.0f, 0.0f);
        theme.shapeHover.xyz = Color(1.0f, 1.0f, 1.0f, 0.2f);
        setTheme(theme);

        createTransform();

        if (_layer) {
            for (auto& line : _guideAngleLines) {
                line = std::make_unique<MeshLine>(_engine, _layer->id());
                if (line->entity()) {
                    line->entity()->setEnabled(false);
                }
            }
        }

        on(EVENT_TRANSFORMSTART, [this](const Vector3& point, const float x, const float y) {
            // store start screen point
            _screenPos = Vector2(x, y);
            _screenStartPos = Vector2(x, y);

            // store start angle
            _selectionStartAngle = calculateArcAngle(point, x, y);

            // store initial node rotations
            storeNodeRotations();

            // store guide points
            storeGuidePoints();

            // drag handle for disk (arc <-> circle)
            drag(true);

            // angle guide lines
            angleGuide(true);
        });

        on(EVENT_TRANSFORMMOVE, [this](const Vector3& point, const float x, const float y) {
            const GizmoAxis axis = _selectedAxis;
            if (axis == GizmoAxis::None) {
                return;
            }

            // update screen point
            _screenPos = Vector2(x, y);

            if (axis == GizmoAxis::XYZ) {
                // calculate angle axis and delta and update node rotations
                const Vector3 delta = point - _selectionStartPoint;
                const Vector3 angleAxis = facingDir().cross(delta).normalized();
                const float angleDelta = std::sqrt((_screenPos - _screenStartPos).lengthSquared());
                setNodeRotations(axis, angleAxis, angleDelta);
            } else {
                // calculate angle axis and delta and update node rotations
                float angleDelta = calculateArcAngle(point, x, y) - _selectionStartAngle;
                if (snap) {
                    angleDelta = std::round(angleDelta / snapIncrement) * snapIncrement;
                }
                setNodeRotations(axis, dirFromAxis(axis), angleDelta);

                // update guide points and show angle guide
                updateGuidePoints(angleDelta);
                angleGuide(true);
            }
        });

        on(EVENT_TRANSFORMEND, [this]() {
            // show all shapes
            drag(false);

            // hide angle guide
            angleGuide(false);
        });

        on(EVENT_NODESDETACH, [this]() {
            // reset stored rotations and offsets
            _nodeLocalRotations.clear();
            _nodeRotations.clear();
            _nodeOffsets.clear();
        });
    }

    RotateGizmo::~RotateGizmo()
    {
        RotateGizmo::destroy();
    }

    float RotateGizmo::xyzTubeRadius() const { return _arcs[0]->tubeRadius(); }
    void RotateGizmo::setXyzTubeRadius(const float value) { for (auto* a : _arcs) a->setTubeRadius(value); }
    float RotateGizmo::xyzRingRadius() const { return _arcs[0]->ringRadius(); }
    void RotateGizmo::setXyzRingRadius(const float value) { for (auto* a : _arcs) a->setRingRadius(value); }
    float RotateGizmo::faceTubeRadius() const { return _face->tubeRadius(); }
    void RotateGizmo::setFaceTubeRadius(const float value) { _face->setTubeRadius(value); }
    float RotateGizmo::faceRingRadius() const { return _face->ringRadius(); }
    void RotateGizmo::setFaceRingRadius(const float value) { _face->setRingRadius(value); }
    float RotateGizmo::centerRadius() const { return _center->radius(); }
    void RotateGizmo::setCenterRadius(const float value) { _center->setRadius(value); }
    float RotateGizmo::ringTolerance() const { return _arcs[0]->tolerance(); }

    void RotateGizmo::setRingTolerance(const float value)
    {
        for (auto* a : _arcs) {
            a->setTolerance(value);
        }
        _face->setTolerance(value);
    }

    float RotateGizmo::angleGuideThickness() const
    {
        return _guideAngleLines[0] ? _guideAngleLines[0]->thickness() : 0.02f;
    }

    void RotateGizmo::setAngleGuideThickness(const float value)
    {
        for (auto& line : _guideAngleLines) {
            if (line) {
                line->setThickness(value);
            }
        }
    }

    void RotateGizmo::storeGuidePoints()
    {
        const Vector3 gizmoPos = rootPosition();
        const bool isFacing = _selectedAxis == GizmoAxis::F;
        const float scale = isFacing ? faceRingRadius() : xyzRingRadius();

        _guideAngleStart = (_selectionStartPoint - gizmoPos).normalized() * scale;
        _guideAngleEnd = _guideAngleStart;
    }

    void RotateGizmo::updateGuidePoints(const float angleDelta)
    {
        const GizmoAxis axis = _selectedAxis;
        Vector3 v1;
        if (axis == GizmoAxis::F) {
            v1 = facingDir();
        } else {
            v1 = _rootStartRot * withGizmoComponent(Vector3(0.0f), gizmoAxisIndex(axis), 1.0f);
        }
        _guideAngleEnd = Quaternion::fromAxisAngle(v1, angleDelta) * _guideAngleStart;

        _renderUpdate = true;
    }

    void RotateGizmo::angleGuide(const bool state)
    {
        const GizmoAxis axis = _selectedAxis;
        if (!_guideAngleLines[0] || !_guideAngleLines[1]) {
            return;
        }

        if (state && dragMode != GizmoDragMode::Show && axis != GizmoAxis::XYZ) {
            const Vector3 gizmoPos = rootPosition();
            const Color& baseColor = axis == GizmoAxis::X ? _theme.shapeHover.x :
                axis == GizmoAxis::Y ? _theme.shapeHover.y :
                axis == GizmoAxis::Z ? _theme.shapeHover.z : _theme.shapeHover.f;
            Color startColor = baseColor;
            startColor.a *= 0.3f;
            _guideAngleLines[0]->draw(gizmoPos, _guideAngleStart + gizmoPos, _scale, startColor);
            _guideAngleLines[1]->draw(gizmoPos, _guideAngleEnd + gizmoPos, _scale, baseColor);
            for (auto& line : _guideAngleLines) {
                if (line->entity()) {
                    line->entity()->setEnabled(true);
                }
            }
        } else {
            for (auto& line : _guideAngleLines) {
                if (line->entity()) {
                    line->entity()->setEnabled(false);
                }
            }
        }
    }

    void RotateGizmo::shapesLookAtCamera()
    {
        // face shape
        if (_camera && _camera->camera() && _camera->camera()->projection() == ProjectionType::Perspective) {
            const Vector3 dir = (cameraPosition() - _root->position()).normalized();
            const float elev = std::atan2(-dir.getY(), std::sqrt(dir.getX() * dir.getX() + dir.getZ() * dir.getZ())) *
                RAD_TO_DEG;
            const float azim = std::atan2(-dir.getX(), -dir.getZ()) * RAD_TO_DEG;
            setWorldRotation(_face->entity(), _root, Quaternion::fromEulerAngles(-elev + 90.0f, azim, 0.0f));
        } else {
            // upstream: setEulerAngles(camera Euler angles), then rotateLocal(-90, 0, 0)
            setWorldRotation(_face->entity(), _root, cameraRotation() * Quaternion::fromEulerAngles(-90.0f, 0.0f, 0.0f));
        }

        // axes shapes
        const Vector3 facing = _root->rotation().invert() * facingDir();
        float angle = std::atan2(facing.getZ(), facing.getY()) * RAD_TO_DEG;
        _arcs[0]->entity()->setLocalEulerAngles(0.0f, angle - 90.0f, -90.0f);
        angle = std::atan2(facing.getX(), facing.getZ()) * RAD_TO_DEG;
        _arcs[1]->entity()->setLocalEulerAngles(0.0f, angle, 0.0f);
        angle = std::atan2(facing.getY(), facing.getX()) * RAD_TO_DEG;
        _arcs[2]->entity()->setLocalEulerAngles(90.0f, 0.0f, angle + 90.0f);

        if (!dragging()) {
            // upstream compares the LOCAL facing direction with the root's WORLD axes; the
            // two agree in world space, and the difference is kept.
            const auto show = [&](ArcShape* arc, const Vector3& axis) {
                const float dot = facing.dot(axis);
                const bool sector = 1.0f - std::abs(dot) > RING_FACING_EPSILON;
                arc->show(sector ? ArcShape::Show::Sector : ArcShape::Show::Ring);
            };
            show(_arcs[0], rootRight());
            show(_arcs[1], rootUp());
            show(_arcs[2], rootForward());
        }

        if (!equalsApprox(facing, _facingDir, UPDATE_EPSILON)) {
            _facingDir = facing;
            _renderUpdate = true;
        }
    }

    void RotateGizmo::drag(const bool state)
    {
        for (auto& [key, s] : _shapes) {
            auto* arc = dynamic_cast<ArcShape*>(s.get());
            if (!arc) {
                continue;
            }
            switch (dragMode) {
                case GizmoDragMode::Show:
                    break;
                case GizmoDragMode::Hide:
                    arc->show(state ? (key == _selectedAxis ? ArcShape::Show::Ring : ArcShape::Show::None)
                                    : ArcShape::Show::Sector);
                    break;
                case GizmoDragMode::Selected:
                    arc->show(state ? (key == _selectedAxis ? ArcShape::Show::Ring : ArcShape::Show::Sector)
                                    : ArcShape::Show::Sector);
                    break;
            }
        }

        _renderUpdate = true;
    }

    void RotateGizmo::storeNodeRotations()
    {
        const Vector3 gizmoPos = rootPosition();
        for (GraphNode* node : _nodes) {
            _nodeLocalRotations[node] = node->localRotation();
            _nodeRotations[node] = node->rotation();
            _nodeOffsets[node] = node->position() - gizmoPos;
        }
    }

    void RotateGizmo::setNodeRotations(const GizmoAxis axis, const Vector3& angleAxis, const float angleDelta)
    {
        const Vector3 gizmoPos = rootPosition();

        // calculate rotation from axis and angle
        const Quaternion q1 = Quaternion::fromAxisAngle(angleAxis, angleDelta);

        const bool singleAxis = axis == GizmoAxis::X || axis == GizmoAxis::Y || axis == GizmoAxis::Z;
        for (GraphNode* node : _nodes) {
            if (singleAxis && _coordSpace == GizmoSpace::Local) {
                const auto rot = _nodeLocalRotations.find(node);
                if (rot == _nodeLocalRotations.end()) {
                    continue;
                }
                node->setLocalRotation(rot->second * q1);
            } else {
                const auto rot = _nodeRotations.find(node);
                const auto offset = _nodeOffsets.find(node);
                if (rot == _nodeRotations.end() || offset == _nodeOffsets.end()) {
                    continue;
                }
                node->setRotation(q1 * rot->second);
                node->setPosition(q1 * offset->second + gizmoPos);
            }
        }

        if (_coordSpace == GizmoSpace::Local) {
            updateRotation();
        }
    }

    Vector3 RotateGizmo::screenToPoint(const float x, const float y)
    {
        if (!_camera) {
            return _point;
        }
        const Vector3 mouseWPos = _camera->screenToWorld(x, y, 1.0f);

        const GizmoAxis axis = _selectedAxis;

        Ray ray = createRay(mouseWPos);
        const Plane plane = createPlane(axis, axis == GizmoAxis::F || axis == GizmoAxis::XYZ, false);

        Vector3 hit;
        if (!plane.intersectsRay(ray, &hit)) {
            // if no intersection, try inverting the ray direction
            ray.direction() = -ray.direction();
            if (!plane.intersectsRay(ray, &hit)) {
                // use gizmo position if ray does not intersect to position angle guide correctly
                hit = rootPosition();
            }
        }

        _point = hit;
        return _point;
    }

    float RotateGizmo::calculateArcAngle(const Vector3& point, const float x, const float y) const
    {
        const Vector3 gizmoPos = rootPosition();
        const GizmoAxis axis = _selectedAxis;
        const Plane plane = createPlane(axis, axis == GizmoAxis::F, false);

        float angle = 0.0f;

        // arc angle
        const Vector3 facing = facingDir();
        const float facingDot = plane.normal().dot(facing);

        switch (rotationMode) {
            case RotationMode::Absolute: {
                if (!_camera) {
                    break;
                }
                const Vector3 v2 = _camera->worldToScreen(gizmoPos);
                Vector3 v1;
                if (axis == GizmoAxis::F || facingDot > 1.0f - RING_FACING_EPSILON) {
                    // determine which side of the ring the mouse is on to flip rotation direction
                    v1 = Vector3(_screenStartPos.y >= v2.getY() ? 1.0f : -1.0f,
                                 _screenStartPos.x >= v2.getX() ? -1.0f : 1.0f, 0.0f).normalized();
                } else {
                    // calculate projection vector in world space for rotation axis
                    const Vector3 projDir = plane.normal().cross(facing).normalized();

                    // convert to screen space
                    const Vector3 v3 = _camera->worldToScreen(projDir + gizmoPos);
                    v1 = (v3 - v2).normalized();
                }

                // angle is dot product with mouse position
                angle = v1.dot(Vector3(x, y, 0.0f));
                break;
            }
            case RotationMode::Orbit: {
                // plane facing camera so based on mouse position around gizmo
                Vector3 v1 = point - gizmoPos;

                switch (axis) {
                    case GizmoAxis::X:
                        v1 = _rootStartRot.invert() * v1;
                        angle = std::atan2(v1.getZ(), v1.getY()) * RAD_TO_DEG;
                        break;
                    case GizmoAxis::Y:
                        v1 = _rootStartRot.invert() * v1;
                        angle = std::atan2(v1.getX(), v1.getZ()) * RAD_TO_DEG;
                        break;
                    case GizmoAxis::Z:
                        v1 = _rootStartRot.invert() * v1;
                        angle = std::atan2(v1.getY(), v1.getX()) * RAD_TO_DEG;
                        break;
                    case GizmoAxis::F:
                        // convert to camera space
                        v1 = cameraRotation().invert() * v1;
                        angle = sign(facingDot) * std::atan2(v1.getY(), v1.getX()) * RAD_TO_DEG;
                        break;
                    default:
                        break;
                }

                // intersection point can be behind camera, so need to check to flip angle delta
                const Vector3 dir = (point - cameraPosition()).normalized();
                if (dir.dot(cameraForward()) < 0.0f) {
                    angle += 180.0f;
                }
                break;
            }
        }
        return angle;
    }

    void RotateGizmo::drawGuideLines(const Vector3& pos, const Quaternion& rot, const GizmoAxis activeAxis,
        const bool activeIsPlane)
    {
        for (const GizmoAxis axis : {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z}) {
            if (activeAxis == GizmoAxis::XYZ) {
                continue;
            }
            if (activeIsPlane) {
                if (axis != activeAxis) {
                    drawSpanLine(pos, rot, axis);
                }
            } else if (axis == activeAxis) {
                drawSpanLine(pos, rot, axis);
            }
        }
    }

    void RotateGizmo::prerender()
    {
        TransformGizmo::prerender();

        if (!enabled()) {
            return;
        }

        shapesLookAtCamera();
    }

    void RotateGizmo::releaseGuideAngleLines()
    {
        for (auto& line : _guideAngleLines) {
            line.reset();
        }
    }

    void RotateGizmo::destroy()
    {
        releaseGuideAngleLines();
        TransformGizmo::destroy();
    }
}
