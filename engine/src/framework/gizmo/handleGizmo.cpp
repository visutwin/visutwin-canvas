// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
#include "handleGizmo.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/shape/plane.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/entity.h"
#include "framework/gizmo/overlay/overlayRenderer.h"
#include "framework/gizmo/shape/shapes.h"
#include "scene/camera.h"
#include "scene/meshInstance.h"

namespace visutwin::canvas
{
    namespace
    {
        // the three great circles of a sphere: in the xz plane, turned onto xy and onto yz
        const std::array<Quaternion, 3> SPHERE_RINGS = {
            Quaternion(),
            Quaternion::fromEulerAngles(90.0f, 0.0f, 0.0f),
            Quaternion::fromEulerAngles(0.0f, 0.0f, 90.0f),
        };

        const std::array<Vector3, 6> SPHERE_AXES = {
            Vector3(1.0f, 0.0f, 0.0f), Vector3(-1.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f),
            Vector3(0.0f, -1.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f), Vector3(0.0f, 0.0f, -1.0f),
        };

        const std::array<Vector3, 3> BOX_UNITS = {
            Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f),
        };
        constexpr std::array<const char*, 3> BOX_FACE_LABELS = {"W", "H", "D"};
        constexpr int BOX_FACES = 6;
        constexpr float BOX_MIN_HALF = 0.005f;
        constexpr float BOX_MIN_FACTOR = 0.01f;

        // a box axis no longer than this, padding left out, is flat and keeps its size and centre
        constexpr float BOX_FLAT_HALF = 1e-6f;

        // the sphere outline's great circles show from this radius on screen, in points
        constexpr float RINGS_SIZE = 40.0f;

        // the handles fade in between these radii on screen, in points
        constexpr float DETAIL_FADE_START = 60.0f;
        constexpr float DETAIL_FADE_END = 100.0f;

        // the side of a corner along an axis, -1 or 1
        float boxSide(const int corner, const int axis)
        {
            return (corner & (1 << axis)) ? 1.0f : -1.0f;
        }
    }

    Vector3 gizmoNodeRight(GraphNode* node)
    {
        return Vector3(node->worldTransform().getColumn(0)).normalized();
    }

    Vector3 gizmoNodeUp(GraphNode* node)
    {
        return Vector3(node->worldTransform().getColumn(1)).normalized();
    }

    Vector3 gizmoNodeForward(GraphNode* node)
    {
        return (-Vector3(node->worldTransform().getColumn(2))).normalized();
    }

    HandleGizmo::HandleGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer, const std::string& name)
        : DragGizmo(camera, std::move(layer), name)
    {
        _theme.base = Color(1.0f, 1.0f, 1.0f, 1.0f);
        _theme.occluded = Color(1.0f, 1.0f, 1.0f, 0.2f);
        _theme.fill = Color(1.0f, 1.0f, 1.0f, 0.05f);
        _theme.primary = GIZMO_COLOR_AMBER;
        _theme.secondary = GIZMO_COLOR_CYAN;
        _theme.tertiary = GIZMO_COLOR_MAGENTA;
        _theme.corner = GIZMO_COLOR_YELLOW;
        _theme.backdrop = GIZMO_COLOR_BACKDROP;
        _faceColors = {&_theme.primary, &_theme.secondary, &_theme.tertiary};

        // the base constructor sized the root as a transform gizmo is; the shapes are world sized
        if (_root) {
            _root->setLocalScale(1.0f, 1.0f, 1.0f);
        }
        _renderer = std::make_unique<OverlayRenderer>(this,
            OverlayRendererTheme{&_theme.base, &_theme.occluded, &_theme.fill, &_theme.backdrop});

        on(EVENT_POINTERDOWN, [this](const float x, const float y, MeshInstance* meshInstance) {
            if (_dragging || !meshInstance) {
                return;
            }
            const GizmoHandle* handle = find(meshInstance);
            if (!handle) {
                if (GraphNode* node = _renderer->icons().pick(meshInstance)) {
                    fire(EVENT_ICONSELECT, node);
                }
                return;
            }
            _drag = *handle;
            const auto point = pointerPoint(x, y);
            if (!point) {
                return;
            }
            _dragOffset = _drag.shape->entity()->position() - *point;
            _dragging = true;
            startHandle(_drag);
            fire(EVENT_HANDLESTART, _drag.node, _drag.name);
        });

        on(EVENT_POINTERMOVE, [this](const float x, const float y, MeshInstance* meshInstance) {
            if (!_dragging) {
                const GizmoHandle* handle = find(meshInstance);
                if (handle ? !matches(_hovered, handle->node, handle->name, handle->index) : !_hovered.name.empty()) {
                    // the highlight changes, so a render-on-demand application draws a frame
                    _renderUpdate = true;
                }
                if (handle) {
                    _hovered = *handle;
                } else {
                    _hovered.name.clear();
                }
                return;
            }
            if (const auto point = pointerPoint(x, y)) {
                moveHandle(_drag, *point + _dragOffset);
                fire(EVENT_HANDLEMOVE, _drag.node, _drag.name);
            }
        });

        on(EVENT_POINTERUP, [this]() {
            endDrag();
        });

        on(EVENT_NODESDETACH, [this]() {
            _hovered.name.clear();
            endDrag();
        });
    }

    HandleGizmo::~HandleGizmo()
    {
        HandleGizmo::destroy();
    }

    float HandleGizmo::lineThickness() const
    {
        return _renderer ? _renderer->lineThickness : 2.0f;
    }

    void HandleGizmo::setLineThickness(const float value)
    {
        if (_renderer) {
            _renderer->lineThickness = value;
        }
    }

    float HandleGizmo::handleSize() const
    {
        return _renderer ? _renderer->handleSize : 7.0f;
    }

    void HandleGizmo::setHandleSize(const float value)
    {
        if (_renderer) {
            _renderer->handleSize = value;
        }
    }

    float HandleGizmo::iconSize() const
    {
        return _renderer ? _renderer->iconSize : 24.0f;
    }

    void HandleGizmo::setIconSize(const float value)
    {
        if (_renderer) {
            _renderer->iconSize = value;
        }
    }

    void HandleGizmo::setTheme(const HandleGizmoThemePartial& partial)
    {
        const auto copy = [](Color& target, const std::optional<Color>& value) {
            if (value) {
                target = *value;
            }
        };
        copy(_theme.base, partial.base);
        copy(_theme.occluded, partial.occluded);
        copy(_theme.fill, partial.fill);
        copy(_theme.primary, partial.primary);
        copy(_theme.secondary, partial.secondary);
        copy(_theme.tertiary, partial.tertiary);
        copy(_theme.corner, partial.corner);
        copy(_theme.backdrop, partial.backdrop);
    }

    const std::vector<GizmoReadout>& HandleGizmo::labels() const
    {
        static const std::vector<GizmoReadout> none;
        return _renderer ? _renderer->labels() : none;
    }

    bool HandleGizmo::perspective() const
    {
        return _camera && _camera->camera() && _camera->camera()->projection() == ProjectionType::Perspective;
    }

    bool HandleGizmo::matches(const GizmoHandle& a, const GraphNode* node, const std::string& name, const int index)
    {
        return !a.name.empty() && a.node == node && a.name == name && a.index == index;
    }

    GizmoHandle* HandleGizmo::find(const MeshInstance* meshInstance) const
    {
        if (!meshInstance) {
            return nullptr;
        }
        for (size_t i = 0; i < _placed.count; ++i) {
            GizmoHandle& handle = *_placed.items()[i];
            if (handle.shape && !handle.shape->meshInstances().empty() &&
                handle.shape->meshInstances().front() == meshInstance) {
                return &handle;
            }
        }
        return nullptr;
    }

    std::optional<Vector3> HandleGizmo::pointerPoint(const float x, const float y) const
    {
        if (!_camera) {
            return std::nullopt;
        }
        const Vector3& origin = _drag.origin;
        const Vector3& dir = _drag.dir;
        const Ray ray = createRay(_camera->screenToWorld(x, y, 1.0f));
        Plane plane;
        Vector3 hit;
        if (_drag.plane) {
            plane.setFromPointNormal(origin, dir);
            if (!plane.intersectsRay(ray, &hit)) {
                return std::nullopt;
            }
            return hit;
        }

        const Vector3 facing = perspective() ? (cameraPosition() - origin).normalized() : -cameraForward();
        Vector3 normal;
        if (!lineNormal(dir, facing, normal)) {
            return std::nullopt;
        }
        plane.setFromPointNormal(origin, normal);
        if (!plane.intersectsRay(ray, &hit)) {
            return std::nullopt;
        }
        const float along = (hit - origin).dot(dir);
        return dir * along + origin;
    }

    void HandleGizmo::endDrag()
    {
        if (!_dragging) {
            return;
        }
        _dragging = false;
        _renderUpdate = true;
        fire(EVENT_HANDLEEND, _drag.node, _drag.name);
        _drag.name.clear();
    }

    bool HandleGizmo::detailed(const Vector3& center, const float radius)
    {
        // fade in rather than pop in
        const float size = _renderer->screenSize(center, radius);
        _detailFade = _dragging ? 1.0f
            : std::clamp((size - DETAIL_FADE_START) / (DETAIL_FADE_END - DETAIL_FADE_START), 0.0f, 1.0f);
        return _detailFade > 0.0f;
    }

    bool HandleGizmo::handle(const Vector3& position, const Color& color, GraphNode* node, const std::string& name,
        const int index, const Vector3& origin, const Vector3& dir, const bool inPlane, const std::optional<bool> guide)
    {
        if (!handles) {
            return false;
        }
        const bool hovered = active(node, name, index);

        GizmoHandle& placed = _placed.next();
        placed.shape = _renderer->handle(position, hovered ? &color : nullptr);
        placed.shape->setFade(hovered ? 1.0f : _detailFade);

        if (hovered && guide.value_or(!inPlane)) {
            _renderer->dashed(origin, position, 1.0f, 1.0f, &color);
        }
        placed.node = node;
        placed.name = name;
        placed.index = index;
        placed.origin = origin;
        placed.dir = dir;
        placed.plane = inPlane;
        return hovered;
    }

    std::string HandleGizmo::readoutValue(const std::string& label, const Color& color, const float value,
        const std::string& unit) const
    {
        char number[32];
        std::snprintf(number, sizeof(number), "%.2f", value);
        return labelMarkup(label, color) + " " + number + unit;
    }

    void HandleGizmo::readoutAt(const Vector3& position, const std::string& label, const Color& color,
        const float value, const std::string& unit)
    {
        readoutMarkupAt(position, readoutValue(label, color, value, unit));
    }

    void HandleGizmo::readoutMarkupAt(const Vector3& position, const std::string& markup)
    {
        if (readout) {
            _renderer->label(position, markup, _renderer->handleSize);
        }
    }

    bool HandleGizmo::active(const GraphNode* node, const std::string& name, const int index) const
    {
        return matches(_dragging ? _drag : _hovered, node, name, index);
    }

    void HandleGizmo::sphereRings(const Vector3& center, const Quaternion& rotation, const float radius,
        const float weight, const float alpha)
    {
        for (const Quaternion& ring : SPHERE_RINGS) {
            _renderer->ring(center, rotation * ring, radius, weight, alpha);
        }
    }

    void HandleGizmo::sphereOutline(const Vector3& center, const Quaternion& rotation, const float radius)
    {
        const Vector3 toCamera = cameraPosition() - center;
        const float dist = toCamera.length();
        if (perspective() && dist <= radius) {
            sphereRings(center, rotation, radius);
            return;
        }
        if (_renderer->screenSize(center, radius) > RINGS_SIZE) {
            sphereRings(center, rotation, radius, 0.5f, 0.35f);
        }
        Vector3 outline;
        const float size = sphereSilhouette(center, radius, outline);
        _renderer->circle(outline, perspective() ? toCamera : cameraForward(), size, 1.1f);
    }

    float HandleGizmo::sphereSilhouette(const Vector3& center, const float radius, Vector3& out) const
    {
        if (!perspective()) {
            out = center;
            return radius;
        }
        const Vector3 toCamera = cameraPosition() - center;
        const float ratio = radius / toCamera.length();
        out = toCamera * (ratio * ratio) + center;
        return radius * std::sqrt(1.0f - ratio * ratio);
    }

    void HandleGizmo::sphereHandles(const Vector3& center, const Quaternion& rotation, const float radius,
        GraphNode* node, const std::string& name)
    {
        for (size_t i = 0; i < SPHERE_AXES.size(); ++i) {
            const Vector3 dir = rotation * SPHERE_AXES[i];
            const Vector3 pos = dir * radius + center;
            if (handle(pos, _theme.primary, node, name, static_cast<int>(i), center, dir)) {
                readoutAt(pos, "r", _theme.primary, radius);
            }
        }
    }

    const std::array<Vector3, 8>& HandleGizmo::boxCorners(const Vector3& center, const Quaternion& rotation,
        const Vector3& halfExtents)
    {
        for (size_t k = 0; k < 3; ++k) {
            _boxAxes[k] = rotation * BOX_UNITS[k];
        }
        for (int i = 0; i < 8; ++i) {
            Vector3 corner = center;
            for (int k = 0; k < 3; ++k) {
                corner = corner + _boxAxes[static_cast<size_t>(k)] * (boxSide(i, k) * halfExtents[k]);
            }
            _boxCorners[static_cast<size_t>(i)] = corner;
        }
        return _boxCorners;
    }

    void HandleGizmo::boxHandles(const Vector3& center, const Vector3& halfExtents, const BoxHandleOptions& options,
        const float pad)
    {
        if (!detailed(center, std::max({halfExtents.getX(), halfExtents.getY(), halfExtents.getZ()}))) {
            return;
        }
        if (options.faceHandles) {
            for (int i = 0; i < BOX_FACES; ++i) {
                const int k = i >> 1;
                if (halfExtents[k] - pad <= BOX_FLAT_HALF) {
                    continue;
                }
                const Vector3 dir = _boxAxes[static_cast<size_t>(k)] * ((i & 1) ? -1.0f : 1.0f);
                const Vector3 pos = dir * halfExtents[k] + center;
                const Color& color = *_faceColors[static_cast<size_t>(k)];
                if (handle(pos, color, nullptr, "size", i, center, dir)) {
                    readoutAt(pos, BOX_FACE_LABELS[static_cast<size_t>(k)], color, 2.0f * (halfExtents[k] - pad));
                }
            }
        }
        if (options.cornerHandles) {
            // a flat box's corners move in its plane, so they stay under the pointer
            int flat = -1;
            for (int k = 0; k < 3; ++k) {
                if (halfExtents[k] - pad <= BOX_FLAT_HALF) {
                    flat = k;
                    break;
                }
            }
            const Vector3 normal = flat < 0 ? cameraForward() : _boxAxes[static_cast<size_t>(flat)];
            for (int i = 0; i < 8; ++i) {
                const Vector3 pos = _boxCorners[static_cast<size_t>(i)];
                const bool hovered = options.uniform
                    ? handle(pos, _theme.corner, nullptr, "size", BOX_FACES + i, center, (pos - center).normalized())
                    : handle(pos, _theme.corner, nullptr, "size", BOX_FACES + i, pos, normal, true);
                if (hovered) {
                    std::string markup;
                    for (int k = 0; k < 3; ++k) {
                        const float half = halfExtents[k] - pad;
                        if (half > BOX_FLAT_HALF) {
                            if (!markup.empty()) {
                                markup += "  ";
                            }
                            markup += readoutValue(BOX_FACE_LABELS[static_cast<size_t>(k)],
                                *_faceColors[static_cast<size_t>(k)], 2.0f * half);
                        }
                    }
                    readoutMarkupAt(pos, markup);
                }
            }
        }
    }

    void HandleGizmo::resizeBox(const GizmoHandle& handle, const Vector3& point, const Quaternion& rotation,
        const Vector3& startPosition, const Vector3& startHalfExtents, const BoxHandleOptions& options, const float pad,
        Vector3& position, Vector3& halfExtents) const
    {
        Vector3 local = rotation.invert() * (point - startPosition);
        const Vector3& start = startHalfExtents;
        Vector3 half = start;
        Vector3 mid(0.0f);
        const bool centered = options.centered;

        // a half extent snapped so the extent without padding is a multiple of the increment
        const auto snapBoxHalf = [this, pad](const float value) {
            return snap ? 0.5f * snapLength(2.0f * (value - pad)) + pad : value;
        };
        // resizes one axis to a dragged face or corner on side `sign`
        const auto resizeAxis = [&](const int axis, const float sign) {
            if (centered) {
                half = withGizmoComponent(half, axis, snapBoxHalf(std::max(sign * local[axis], BOX_MIN_HALF)));
                mid = withGizmoComponent(mid, axis, 0.0f);
                return;
            }
            const float anchor = -sign * start[axis];
            const float h = snapBoxHalf(0.5f * std::max(sign * (local[axis] - anchor), 2.0f * BOX_MIN_HALF));
            half = withGizmoComponent(half, axis, h);
            mid = withGizmoComponent(mid, axis, anchor + sign * h);
        };

        // a corner moves in the view plane, so a flat axis ignores its drag
        const int corner = handle.index - BOX_FACES;
        if (corner >= 0) {
            for (int k = 0; k < 3; ++k) {
                if (start[k] - pad <= BOX_FLAT_HALF) {
                    local = withGizmoComponent(local, k, boxSide(corner, k) * start[k]);
                }
            }
        }

        if (handle.index < BOX_FACES) {
            resizeAxis(handle.index >> 1, (handle.index & 1) ? -1.0f : 1.0f);
        } else if (options.uniform) {
            Vector3 anchor = centered ? Vector3(0.0f) : Vector3(-boxSide(corner, 0) * start.getX(),
                -boxSide(corner, 1) * start.getY(), -boxSide(corner, 2) * start.getZ());
            const float from = std::hypot(boxSide(corner, 0) * start.getX() - anchor.getX(),
                boxSide(corner, 1) * start.getY() - anchor.getY(), boxSide(corner, 2) * start.getZ() - anchor.getZ());
            float factor = std::max(local.distance(anchor) / from, BOX_MIN_FACTOR);
            if (snap) {
                // snap the largest extent and keep the proportions
                const float largest = std::max({start.getX(), start.getY(), start.getZ()});
                factor = snapBoxHalf(largest * factor) / largest;
            }
            half = half * factor;
            mid = anchor * (1.0f - factor);
        } else {
            for (int k = 0; k < 3; ++k) {
                resizeAxis(k, boxSide(corner, k));
            }
        }

        // a flat axis can't scale, so it keeps its size and centre
        if (corner >= 0) {
            for (int k = 0; k < 3; ++k) {
                if (start[k] - pad <= BOX_FLAT_HALF) {
                    half = withGizmoComponent(half, k, start[k]);
                    mid = withGizmoComponent(mid, k, 0.0f);
                }
            }
        }

        halfExtents = half;
        position = rotation * mid + startPosition;
    }

    void HandleGizmo::placeIcon(const OverlayIcon kind, const Vector3& position, GraphNode* node)
    {
        if (icon) {
            _renderer->icons().add(kind, position, node);
        }
    }

    void HandleGizmo::prerender()
    {
        _placed.count = 0;
        if (!_renderer) {
            return;
        }
        if (!enabled()) {
            _renderer->clear();
            return;
        }
        _renderer->begin();
        drawShapes();
        _renderer->end();
    }

    void HandleGizmo::destroy()
    {
        if (_renderer) {
            _renderer->destroy();
        }
        DragGizmo::destroy();
    }
}
