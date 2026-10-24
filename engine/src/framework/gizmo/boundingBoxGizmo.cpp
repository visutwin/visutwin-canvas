// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.10.2026
//
#include "boundingBoxGizmo.h"

#include <algorithm>
#include <cmath>

#include "framework/components/render/renderComponent.h"
#include "framework/entity.h"
#include "framework/gizmo/overlay/overlayRenderer.h"
#include "scene/meshInstance.h"

namespace visutwin::canvas
{
    namespace
    {
        // the origin marker: a dot of this radius in a ring of this radius, in points
        constexpr float ORIGIN_DOT = 3.0f;
        constexpr float ORIGIN_RING = 7.0f;
        constexpr float MIN_FACTOR = 0.01f;
        constexpr float MIN_EXTENT = 1e-6f;

        // the world axis nearest a direction
        int nearest(const Vector3& dir)
        {
            const float x = std::abs(dir.getX());
            const float y = std::abs(dir.getY());
            const float z = std::abs(dir.getZ());
            return x >= y && x >= z ? 0 : y >= z ? 1 : 2;
        }

        // the factor an extent was resized by, padding left out; a flat extent keeps its size
        float fitFactor(const float half, const float start, const float pad)
        {
            return start - pad > MIN_EXTENT ? std::max((half - pad) / (start - pad), MIN_FACTOR) : 1.0f;
        }
    }

    BoundingBoxGizmo::BoundingBoxGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer)
        : HandleGizmo(camera, std::move(layer), "gizmo:bounding-box")
    {
    }

    void BoundingBoxGizmo::add(const BoundingBox& aabb)
    {
        if (!_empty) {
            _bounds.add(aabb);
            return;
        }
        _bounds = aabb;
        _empty = false;
    }

    void BoundingBoxGizmo::addNode(GraphNode* node)
    {
        if (!node || !node->enabled()) {
            return;
        }
        if (auto* entity = dynamic_cast<Entity*>(node)) {
            if (auto* render = entity->findComponent<RenderComponent>(); render && render->enabled()) {
                for (MeshInstance* meshInstance : render->meshInstances()) {
                    if (meshInstance && meshInstance->visible()) {
                        add(meshInstance->aabb());
                    }
                }
            }
        }
        node->forEachChild([this](GraphNode* child) { addNode(child); });
    }

    void BoundingBoxGizmo::drawShapes()
    {
        _empty = true;
        for (GraphNode* node : _nodes) {
            addNode(node);
        }
        if (!_empty) {
            // a resize changes the nodes, so the box follows their bounds
            _center = _bounds.center();
            _halfExtents = _bounds.halfExtents() + Vector3(padding);
            const auto& corners = boxCorners(_center, Quaternion(), _halfExtents);
            drawBrackets(corners);
            if (_theme.fill.a > 0.0f) {
                // the bounds enclose other gizmos' shapes, so the fill leaves the depth to them
                _renderer->box(_center, Quaternion(), _halfExtents * 2.0f)->setDepthWrite(false);
            }
            boxHandles(_center, _halfExtents, *this, padding);
        }
        if (icon) {
            for (GraphNode* node : _nodes) {
                if (node->enabled()) {
                    const Vector3 pos = node->position();
                    _renderer->dot(pos, ORIGIN_DOT);
                    _renderer->circle(pos, cameraForward(), _renderer->pixels(pos, ORIGIN_RING), 0.6f);
                }
            }
        }
    }

    void BoundingBoxGizmo::drawBrackets(const std::array<Vector3, 8>& corners)
    {
        const Vector3& c = _center;
        const Vector3& h = _halfExtents;
        for (int x = -1; x <= 1; x += 2) {
            for (int y = -1; y <= 1; y += 2) {
                for (int z = -1; z <= 1; z += 2) {
                    const Vector3 corner(c.getX() + h.getX() * static_cast<float>(x),
                        c.getY() + h.getY() * static_cast<float>(y), c.getZ() + h.getZ() * static_cast<float>(z));
                    for (int k = 0; k < 3; ++k) {
                        const float sign = static_cast<float>(k == 0 ? x : k == 1 ? y : z);
                        _renderer->arm(corner, k, sign, h[k] * armLength, armThickness);
                    }
                }
            }
        }
        if (edges) {
            for (int i = 0; i < 8; ++i) {
                for (int bit = 1; bit < 8; bit <<= 1) {
                    if (!(i & bit)) {
                        _renderer->dashed(corners[static_cast<size_t>(i)], corners[static_cast<size_t>(i | bit)],
                            0.5f, 0.45f);
                    }
                }
            }
        }
    }

    void BoundingBoxGizmo::startHandle(const GizmoHandle& /*handle*/)
    {
        _startCenter = _center;
        _startHalfExtents = _halfExtents;
        _startPositions.resize(_nodes.size());
        _startScales.resize(_nodes.size());
        for (size_t i = 0; i < _nodes.size(); ++i) {
            _startPositions[i] = _nodes[i]->position();
            _startScales[i] = _nodes[i]->localScale();
        }
    }

    void BoundingBoxGizmo::moveHandle(const GizmoHandle& handle, const Vector3& point)
    {
        resizeBox(handle, point, Quaternion(), _startCenter, _startHalfExtents, *this, padding, _center, _halfExtents);
        const Vector3& half = _halfExtents;
        const Vector3& start = _startHalfExtents;
        const float factor[3] = {fitFactor(half.getX(), start.getX(), padding),
            fitFactor(half.getY(), start.getY(), padding), fitFactor(half.getZ(), start.getZ(), padding)};
        const Vector3 factors(factor[0], factor[1], factor[2]);
        for (size_t i = 0; i < _nodes.size() && i < _startPositions.size(); ++i) {
            GraphNode* node = _nodes[i];
            node->setPosition((_startPositions[i] - _startCenter) * factors + _center);

            // each local axis scales by the factor of its nearest world axis
            const Vector3& scale = _startScales[i];
            node->setLocalScale(scale.getX() * factor[nearest(gizmoNodeRight(node))],
                scale.getY() * factor[nearest(gizmoNodeUp(node))],
                scale.getZ() * factor[nearest(gizmoNodeForward(node))]);
        }
    }
}
