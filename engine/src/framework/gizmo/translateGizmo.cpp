// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "translateGizmo.h"

#include <cmath>

#include "framework/components/camera/cameraComponent.h"
#include "framework/entity.h"
#include "framework/gizmo/shape/shapes.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr float GLANCE_EPSILON = 0.01f;

        bool isSingleAxis(const GizmoAxis key)
        {
            return key == GizmoAxis::X || key == GizmoAxis::Y || key == GizmoAxis::Z || key == GizmoAxis::F;
        }

        Vector3 roundVector(const Vector3& v)
        {
            return Vector3(std::round(v.getX()), std::round(v.getY()), std::round(v.getZ()));
        }
    }

    TranslateGizmo::TranslateGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer)
        : TransformGizmo(camera, std::move(layer), "gizmo:translate")
    {
        snapIncrement = 1.0f;

        _center = addShape<SphereShape>(GizmoAxis::XYZ, _engine, shapeArgs(GizmoAxis::XYZ));
        const auto planeArgs = [this](const GizmoAxis axis, const Vector3& rotation) {
            ShapeArgs args = shapeArgs(axis, rotation);
            args.depth = 1.0f;
            return args;
        };
        _planes[0] = addShape<PlaneShape>(GizmoAxis::YZ, _engine, planeArgs(GizmoAxis::X, Vector3(0.0f, 0.0f, -90.0f)));
        _planes[1] = addShape<PlaneShape>(GizmoAxis::XZ, _engine, planeArgs(GizmoAxis::Y, Vector3(0.0f, 0.0f, 0.0f)));
        _planes[2] = addShape<PlaneShape>(GizmoAxis::XY, _engine, planeArgs(GizmoAxis::Z, Vector3(90.0f, 0.0f, 0.0f)));
        _arrows[0] = addShape<ArrowShape>(GizmoAxis::X, _engine, shapeArgs(GizmoAxis::X, Vector3(0.0f, 0.0f, -90.0f)));
        _arrows[1] = addShape<ArrowShape>(GizmoAxis::Y, _engine, shapeArgs(GizmoAxis::Y, Vector3(0.0f, 0.0f, 0.0f)));
        _arrows[2] = addShape<ArrowShape>(GizmoAxis::Z, _engine, shapeArgs(GizmoAxis::Z, Vector3(90.0f, 0.0f, 0.0f)));

        createTransform();

        on(EVENT_TRANSFORMSTART, [this]() {
            // store the initial positions of the nodes
            storeNodePositions();

            // hide shapes that are not selected
            drag(true);
        });

        on(EVENT_TRANSFORMMOVE, [this](const Vector3& point) {
            // calculate translate delta and update node positions
            Vector3 translateDelta = point - _selectionStartPoint;
            if (snap) {
                translateDelta = roundVector(translateDelta * (1.0f / snapIncrement)) * snapIncrement;
            }
            setNodePositions(translateDelta);
        });

        on(EVENT_TRANSFORMEND, [this]() {
            // show all shapes
            drag(false);
        });

        on(EVENT_NODESDETACH, [this]() {
            // reset stored positions
            _nodeLocalPositions.clear();
            _nodePositions.clear();
        });
    }

    TranslateGizmo::~TranslateGizmo()
    {
        TranslateGizmo::destroy();
    }

    float TranslateGizmo::axisGap() const { return _arrows[0]->gap(); }
    void TranslateGizmo::setAxisGap(const float value) { for (auto* a : _arrows) a->setGap(value); }
    float TranslateGizmo::axisLineThickness() const { return _arrows[0]->lineThickness(); }
    void TranslateGizmo::setAxisLineThickness(const float value) { for (auto* a : _arrows) a->setLineThickness(value); }
    float TranslateGizmo::axisLineLength() const { return _arrows[0]->lineLength(); }
    void TranslateGizmo::setAxisLineLength(const float value) { for (auto* a : _arrows) a->setLineLength(value); }
    float TranslateGizmo::axisLineTolerance() const { return _arrows[0]->tolerance(); }
    void TranslateGizmo::setAxisLineTolerance(const float value) { for (auto* a : _arrows) a->setTolerance(value); }
    float TranslateGizmo::axisArrowThickness() const { return _arrows[0]->arrowThickness(); }
    void TranslateGizmo::setAxisArrowThickness(const float value) { for (auto* a : _arrows) a->setArrowThickness(value); }
    float TranslateGizmo::axisArrowLength() const { return _arrows[0]->arrowLength(); }
    void TranslateGizmo::setAxisArrowLength(const float value) { for (auto* a : _arrows) a->setArrowLength(value); }
    float TranslateGizmo::axisPlaneSize() const { return _planes[0]->size(); }
    void TranslateGizmo::setAxisPlaneSize(const float value) { for (auto* p : _planes) p->setSize(value); }
    float TranslateGizmo::axisPlaneGap() const { return _planes[0]->gap(); }
    void TranslateGizmo::setAxisPlaneGap(const float value) { for (auto* p : _planes) p->setGap(value); }
    float TranslateGizmo::axisCenterSize() const { return _center->radius(); }
    void TranslateGizmo::setAxisCenterSize(const float value) { _center->setRadius(value); }

    void TranslateGizmo::shapesLookAtCamera()
    {
        const Vector3 dir = cameraDir();
        const Vector3 right = rootRight();
        const Vector3 up = rootUp();
        const Vector3 forward = rootForward();

        // axes
        bool changed = false;
        const auto setEnabled = [&changed](Shape* s, const bool enabled) {
            if (s->entity()->enabledLocal() != enabled) {
                s->entity()->setEnabled(enabled);
                changed = true;
            }
        };
        setEnabled(_arrows[0], 1.0f - std::abs(dir.dot(right)) > GLANCE_EPSILON);
        setEnabled(_arrows[1], 1.0f - std::abs(dir.dot(up)) > GLANCE_EPSILON);
        setEnabled(_arrows[2], 1.0f - std::abs(dir.dot(forward)) > GLANCE_EPSILON);

        // planes
        const auto setFlipped = [&changed](PlaneShape* plane, const Vector3& flipped) {
            if (!gizmoVectorEquals(plane->flipped(), flipped)) {
                plane->setFlipped(flipped);
                changed = true;
            }
        };
        const auto b = [](const bool v) { return v ? 1.0f : 0.0f; };

        Vector3 v1 = dir.cross(right);
        setEnabled(_planes[0], 1.0f - v1.length() > GLANCE_EPSILON);
        setFlipped(_planes[0], flipPlanes ? Vector3(0.0f, b(v1.dot(forward) < 0.0f), b(v1.dot(up) < 0.0f)) : Vector3(0.0f));

        v1 = dir.cross(forward);
        setEnabled(_planes[2], 1.0f - v1.length() > GLANCE_EPSILON);
        setFlipped(_planes[2], flipPlanes ? Vector3(b(v1.dot(up) < 0.0f), b(v1.dot(right) > 0.0f), 0.0f) : Vector3(0.0f));

        v1 = dir.cross(up);
        setEnabled(_planes[1], 1.0f - v1.length() > GLANCE_EPSILON);
        setFlipped(_planes[1], flipPlanes ? Vector3(b(v1.dot(forward) > 0.0f), 0.0f, b(v1.dot(right) > 0.0f)) : Vector3(0.0f));

        if (changed) {
            _renderUpdate = true;
        }
    }

    void TranslateGizmo::drag(const bool state)
    {
        for (auto& [key, s] : _shapes) {
            switch (dragMode) {
                case GizmoDragMode::Show:
                    continue;
                case GizmoDragMode::Hide:
                    s->setVisible(!state);
                    continue;
                case GizmoDragMode::Selected:
                    if (_selectedAxis == GizmoAxis::XYZ) {
                        s->setVisible(state ? isSingleAxis(key) : true);
                        continue;
                    }
                    if (_selectedIsPlane) {
                        s->setVisible(state ? isSingleAxis(key) && key != _selectedAxis : true);
                        continue;
                    }
                    s->setVisible(state ? key == _selectedAxis : true);
            }
        }

        _renderUpdate = true;
    }

    void TranslateGizmo::storeNodePositions()
    {
        for (GraphNode* node : _nodes) {
            _nodeLocalPositions[node] = node->localPosition();
            _nodePositions[node] = node->position();
        }
    }

    void TranslateGizmo::setNodePositions(const Vector3& translateDelta)
    {
        for (GraphNode* node : _nodes) {
            if (_coordSpace == GizmoSpace::Local) {
                const auto it = _nodeLocalPositions.find(node);
                if (it == _nodeLocalPositions.end()) {
                    continue;
                }
                Vector3 parentScale(1.0f);
                if (node->parent()) {
                    parentScale = node->parent()->worldTransform().getScale();
                }
                const Vector3 delta = (node->localRotation() * translateDelta) / parentScale;
                node->setLocalPosition(delta + it->second);
            } else {
                const auto it = _nodePositions.find(node);
                if (it == _nodePositions.end()) {
                    continue;
                }
                node->setPosition(translateDelta + it->second);
            }
        }

        updatePosition();
    }

    Vector3 TranslateGizmo::screenToPoint(const float x, const float y)
    {
        if (!_camera) {
            return _point;
        }
        const Vector3 mouseWPos = _camera->screenToWorld(x, y, 1.0f);

        const GizmoAxis axis = _selectedAxis;
        const bool isPlane = _selectedIsPlane;

        const Ray ray = createRay(mouseWPos);
        const Plane plane = createPlane(axis, axis == GizmoAxis::XYZ, !isPlane);
        Vector3 hit;
        if (!plane.intersectsRay(ray, &hit)) {
            return _point;
        }

        // rotate point back to world coords
        hit = _rootStartRot.invert() * hit;

        // project point onto axis
        if (!isPlane && axis != GizmoAxis::XYZ) {
            hit = projectToAxis(hit, axis);
        }

        _point = hit;
        return _point;
    }

    void TranslateGizmo::drawGuideLines(const Vector3& pos, const Quaternion& rot, const GizmoAxis activeAxis,
        const bool activeIsPlane)
    {
        for (const GizmoAxis axis : {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z}) {
            if (dragging() || activeAxis == GizmoAxis::XYZ) {
                drawSpanLine(pos, rot, axis);
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

    void TranslateGizmo::prerender()
    {
        TransformGizmo::prerender();

        if (!enabled()) {
            return;
        }

        shapesLookAtCamera();
    }
}
