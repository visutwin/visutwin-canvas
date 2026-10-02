// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
#include "scaleGizmo.h"

#include <cmath>

#include "framework/components/camera/cameraComponent.h"
#include "framework/entity.h"
#include "framework/gizmo/shape/shapes.h"

namespace visutwin::canvas
{
    namespace
    {


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
        updateAxisAndPlaneShapes({_lines[0], _lines[1], _lines[2]}, _planes, flipPlanes);
    }

    void ScaleGizmo::drag(const bool state)
    {
        applyAxisDragVisibility(state);
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
