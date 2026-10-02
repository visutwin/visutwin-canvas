// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "scaleGizmo.h"

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

    ScaleGizmo::ScaleGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer)
        : TransformGizmo(camera, std::move(layer), "gizmo:scale")
    {
        _coordSpace = GizmoSpace::Local;
        snapIncrement = 1.0f;

        _center = addShape<BoxShape>(GizmoAxis::XYZ, _engine, shapeArgs(GizmoAxis::XYZ));
        const auto planeArgs = [this](const GizmoAxis axis, const Vector3& rotation) {
            ShapeArgs args = shapeArgs(axis, rotation);
            args.depth = 1.0f;
            return args;
        };
        _planes[0] = addShape<PlaneShape>(GizmoAxis::YZ, _engine, planeArgs(GizmoAxis::X, Vector3(0.0f, 0.0f, -90.0f)));
        _planes[1] = addShape<PlaneShape>(GizmoAxis::XZ, _engine, planeArgs(GizmoAxis::Y, Vector3(0.0f, 0.0f, 0.0f)));
        _planes[2] = addShape<PlaneShape>(GizmoAxis::XY, _engine, planeArgs(GizmoAxis::Z, Vector3(90.0f, 0.0f, 0.0f)));
        _lines[0] = addShape<BoxLineShape>(GizmoAxis::X, _engine, shapeArgs(GizmoAxis::X, Vector3(0.0f, 0.0f, -90.0f)));
        _lines[1] = addShape<BoxLineShape>(GizmoAxis::Y, _engine, shapeArgs(GizmoAxis::Y, Vector3(0.0f, 0.0f, 0.0f)));
        _lines[2] = addShape<BoxLineShape>(GizmoAxis::Z, _engine, shapeArgs(GizmoAxis::Z, Vector3(90.0f, 0.0f, 0.0f)));

        createTransform();

        on(EVENT_TRANSFORMSTART, [this]() {
            // store initial scales of nodes
            storeNodeScales();

            // hide shapes that are not selected
            drag(true);
        });

        on(EVENT_TRANSFORMMOVE, [this](const Vector3& point) {
            // calculate scale delta and update node scales
            Vector3 scaleDelta = point - _selectionStartPoint;
            if (snap) {
                scaleDelta = roundVector(scaleDelta * (1.0f / snapIncrement)) * snapIncrement;
            }
            scaleDelta = scaleDelta * (1.0f / _scale);
            setNodeScales(scaleDelta + Vector3(1.0f));
        });

        on(EVENT_TRANSFORMEND, [this]() {
            // show all shapes
            drag(false);
        });

        on(EVENT_NODESDETACH, [this]() {
            // reset stored scales
            _nodeScales.clear();
        });
    }

    ScaleGizmo::~ScaleGizmo()
    {
        ScaleGizmo::destroy();
    }

    float ScaleGizmo::axisGap() const { return _lines[0]->gap(); }
    void ScaleGizmo::setAxisGap(const float value) { for (auto* l : _lines) l->setGap(value); }
    float ScaleGizmo::axisLineThickness() const { return _lines[0]->lineThickness(); }
    void ScaleGizmo::setAxisLineThickness(const float value) { for (auto* l : _lines) l->setLineThickness(value); }
    float ScaleGizmo::axisLineLength() const { return _lines[0]->lineLength(); }
    void ScaleGizmo::setAxisLineLength(const float value) { for (auto* l : _lines) l->setLineLength(value); }
    float ScaleGizmo::axisLineTolerance() const { return _lines[0]->tolerance(); }
    void ScaleGizmo::setAxisLineTolerance(const float value) { for (auto* l : _lines) l->setTolerance(value); }
    float ScaleGizmo::axisBoxSize() const { return _lines[0]->boxSize(); }
    void ScaleGizmo::setAxisBoxSize(const float value) { for (auto* l : _lines) l->setBoxSize(value); }
    float ScaleGizmo::axisPlaneSize() const { return _planes[0]->size(); }
    void ScaleGizmo::setAxisPlaneSize(const float value) { for (auto* p : _planes) p->setSize(value); }
    float ScaleGizmo::axisPlaneGap() const { return _planes[0]->gap(); }
    void ScaleGizmo::setAxisPlaneGap(const float value) { for (auto* p : _planes) p->setGap(value); }
    float ScaleGizmo::axisCenterSize() const { return _center->size(); }
    void ScaleGizmo::setAxisCenterSize(const float value) { _center->setSize(value); }

    void ScaleGizmo::shapesLookAtCamera()
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
        setEnabled(_lines[0], 1.0f - std::abs(dir.dot(right)) > GLANCE_EPSILON);
        setEnabled(_lines[1], 1.0f - std::abs(dir.dot(up)) > GLANCE_EPSILON);
        setEnabled(_lines[2], 1.0f - std::abs(dir.dot(forward)) > GLANCE_EPSILON);

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

    void ScaleGizmo::drag(const bool state)
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

    void ScaleGizmo::storeNodeScales()
    {
        for (GraphNode* node : _nodes) {
            _nodeScales[node] = node->localScale();
        }
    }

    void ScaleGizmo::setNodeScales(const Vector3& scaleDelta)
    {
        for (GraphNode* node : _nodes) {
            const auto it = _nodeScales.find(node);
            if (it == _nodeScales.end()) {
                continue;
            }
            node->setLocalScale(Vector3::max(it->second * scaleDelta, lowerBoundScale));
        }
    }

    Vector3 ScaleGizmo::screenToPoint(const float x, const float y)
    {
        if (!_camera) {
            return _point;
        }
        const Vector3 gizmoPos = rootPosition();
        const Vector3 mouseWPos = _camera->screenToWorld(x, y, 1.0f);

        const GizmoAxis axis = _selectedAxis;
        const bool isPlane = _selectedIsPlane;

        const Ray ray = createRay(mouseWPos);
        const Plane plane = createPlane(axis, axis == GizmoAxis::XYZ, !isPlane);
        Vector3 hit;
        if (!plane.intersectsRay(ray, &hit)) {
            return _point;
        }

        // uniform scaling for XYZ axis
        if (axis == GizmoAxis::XYZ) {
            // calculate projection vector for scale direction
            const Vector3 projDir = (cameraUpAxis() + cameraRightAxis()).normalized();

            // calculate direction vector for scaling
            const Vector3 dir = hit - gizmoPos;

            // normalize vector and project it to scale direction
            const float v = dir.length() * dir.normalized().dot(projDir);
            _point = Vector3(v);
            return _point;
        }

        // rotate point back to world coords
        hit = _rootStartRot.invert() * hit;

        // project point onto axis
        if (!isPlane) {
            hit = projectToAxis(hit, axis);
        }

        // uniform scaling for planes
        if (_uniform && isPlane) {
            // project to diagonal line
            const int index = gizmoAxisIndex(axis);
            const Vector3 diagonal = withGizmoComponent(Vector3(1.0f), index, 0.0f);
            hit = withGizmoComponent(diagonal * diagonal.dot(hit), index, 0.0f);
        }

        _point = hit;
        return _point;
    }

    void ScaleGizmo::prerender()
    {
        TransformGizmo::prerender();

        if (!enabled()) {
            return;
        }

        shapesLookAtCamera();
    }
}
