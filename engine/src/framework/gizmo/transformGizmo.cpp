// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "transformGizmo.h"

#include <algorithm>

#include "framework/components/camera/cameraComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/camera.h"
#include "scene/constants.h"
#include "scene/graphics/wideLineRenderer.h"
#include "scene/layer.h"
#include "scene/meshInstance.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr std::array<GizmoAxis, 3> AXES = {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z};

        Color lerpColor(const Color& a, const Color& b, const float t)
        {
            Color c;
            c.lerp(a, b, t);
            return c;
        }

        const Color* themeColor(const GizmoTheme::AxisColors& colors, const GizmoAxis axis)
        {
            switch (axis) {
                case GizmoAxis::X: return &colors.x;
                case GizmoAxis::Y: return &colors.y;
                case GizmoAxis::Z: return &colors.z;
                case GizmoAxis::XYZ: return &colors.xyz;
                case GizmoAxis::F: return &colors.f;
                default: return nullptr;
            }
        }

        void copyIfSet(Color& target, const std::optional<Color>& value)
        {
            if (value) {
                target = *value;
            }
        }
    }

    TransformGizmo::TransformGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer, const std::string& name)
        : Gizmo(camera, std::move(layer), name)
    {
        // color.js and the theme defaults
        _theme.shapeBase = {GIZMO_COLOR_RED, GIZMO_COLOR_GREEN, GIZMO_COLOR_BLUE,
            Color(0.8f, 0.8f, 0.8f, 1.0f), Color(0.8f, 0.8f, 0.8f, 1.0f)};
        _theme.shapeHover = {lerpColor(GIZMO_COLOR_RED, Color::WHITE, 0.75f),
            lerpColor(GIZMO_COLOR_GREEN, Color::WHITE, 0.75f), lerpColor(GIZMO_COLOR_BLUE, Color::WHITE, 0.75f),
            Color::WHITE, Color::WHITE};
        _theme.guideBase = {GIZMO_COLOR_RED, GIZMO_COLOR_GREEN, GIZMO_COLOR_BLUE};
        _theme.guideOcclusion = 0.8f;
        _theme.disabled = GIZMO_COLOR_GRAY;

        on(EVENT_POINTERDOWN, [this](const float x, const float y, MeshInstance* meshInstance) {
            const Shape* target = shapeOf(meshInstance);
            if (target && target->disabled()) {
                return;
            }
            if (dragging()) {
                return;
            }
            if (!meshInstance) {
                return;
            }

            _hoverAxis = GizmoAxis::None;
            _hoverIsPlane = false;
            _selectedAxis = axisOf(meshInstance);
            _selectedIsPlane = isPlaneOf(meshInstance);

            _rootStartPos = _root->localPosition();
            _rootStartRot = _root->rotation();
            const Vector3 point = screenToPoint(x, y);
            _selectionStartPoint = point;

            fire(EVENT_TRANSFORMSTART, point, x, y);
        });

        on(EVENT_POINTERMOVE, [this](const float x, const float y, MeshInstance* meshInstance) {
            const Shape* target = shapeOf(meshInstance);
            if (target && target->disabled()) {
                return;
            }

            hover(meshInstance);

            if (!dragging()) {
                return;
            }

            const Vector3 point = screenToPoint(x, y);
            fire(EVENT_TRANSFORMMOVE, point, x, y);
        });

        on(EVENT_POINTERUP, [this](const float /*x*/, const float /*y*/, MeshInstance* meshInstance) {
            hover(meshInstance);

            if (!dragging()) {
                return;
            }

            if (meshInstance) {
                _hoverAxis = _selectedAxis;
                _hoverIsPlane = _selectedIsPlane;
            }
            _selectedAxis = GizmoAxis::None;
            _selectedIsPlane = false;

            fire(EVENT_TRANSFORMEND);
        });

        on(EVENT_NODESDETACH, [this]() {
            _hoverAxis = GizmoAxis::None;
            _hoverIsPlane = false;
            hover(nullptr);
            // DEVIATION: upstream fires 'pointer:up' with no arguments; a typed handler here
            // is skipped when its arguments are missing, so the pointer-up carries (0, 0, null).
            fire(EVENT_POINTERUP, 0.0f, 0.0f, static_cast<MeshInstance*>(nullptr));
        });
    }

    TransformGizmo::~TransformGizmo()
    {
        TransformGizmo::destroy();
    }

    ShapeArgs TransformGizmo::shapeArgs(const GizmoAxis axis, const Vector3& rotation) const
    {
        ShapeArgs args;
        args.axis = axis;
        args.rotation = rotation;
        if (_layer) {
            args.layers = {_layer->id()};
        }
        args.defaultColor = themeColor(_theme.shapeBase, axis);
        args.hoverColor = themeColor(_theme.shapeHover, axis);
        args.disabledColor = &_theme.disabled;
        return args;
    }

    Shape* TransformGizmo::shape(const GizmoAxis key) const
    {
        for (const auto& [k, s] : _shapes) {
            if (k == key) {
                return s.get();
            }
        }
        return nullptr;
    }

    Shape* TransformGizmo::shapeOf(const MeshInstance* meshInstance) const
    {
        if (!meshInstance) {
            return nullptr;
        }
        for (const auto& entry : _shapes) {
            if (entry.second->ownsMeshInstance(meshInstance)) {
                return entry.second.get();
            }
        }
        return nullptr;
    }

    GizmoAxis TransformGizmo::axisOf(const MeshInstance* meshInstance) const
    {
        // Upstream reads it from the node name ('arrow:x', 'plane:x', ...).
        const Shape* s = shapeOf(meshInstance);
        return s ? s->axis() : GizmoAxis::None;
    }

    bool TransformGizmo::isPlaneOf(const MeshInstance* meshInstance) const
    {
        const Shape* s = shapeOf(meshInstance);
        return s && s->isPlane();
    }

    void TransformGizmo::hover(const MeshInstance* meshInstance)
    {
        if (dragging()) {
            return;
        }

        // track changes
        std::vector<GizmoAxis> remove = _hovering;
        bool changed = false;
        const auto add = [&](const GizmoAxis axis) {
            if (const auto it = std::find(remove.begin(), remove.end(), axis); it != remove.end()) {
                remove.erase(it);
            } else {
                _hovering.push_back(axis);
                if (Shape* s = shape(axis)) {
                    s->hover(true);
                }
                changed = true;
            }
        };

        // determine which axis is hovered
        _hoverAxis = axisOf(meshInstance);
        _hoverIsPlane = isPlaneOf(meshInstance);

        // add shapes that are hovered
        if (_hoverAxis != GizmoAxis::None) {
            if (_hoverAxis == GizmoAxis::XYZ) {
                add(GizmoAxis::X);
                add(GizmoAxis::Y);
                add(GizmoAxis::Z);
                add(GizmoAxis::XYZ);
            } else if (_hoverIsPlane) {
                switch (_hoverAxis) {
                    case GizmoAxis::X:
                        add(GizmoAxis::Y);
                        add(GizmoAxis::Z);
                        add(GizmoAxis::YZ);
                        break;
                    case GizmoAxis::Y:
                        add(GizmoAxis::X);
                        add(GizmoAxis::Z);
                        add(GizmoAxis::XZ);
                        break;
                    case GizmoAxis::Z:
                        add(GizmoAxis::X);
                        add(GizmoAxis::Y);
                        add(GizmoAxis::XY);
                        break;
                    default:
                        break;
                }
            } else {
                add(_hoverAxis);
            }
        }

        // unhover removed shapes
        for (const GizmoAxis axis : remove) {
            std::erase(_hovering, axis);
            if (Shape* s = shape(axis)) {
                s->hover(false);
            }
            changed = true;
        }

        if (changed) {
            _renderUpdate = true;
        }
    }

    Ray TransformGizmo::createRay(const Vector3& mouseWPos) const
    {
        if (_camera && _camera->camera() && _camera->camera()->projection() == ProjectionType::Perspective) {
            const Vector3 origin = cameraPosition();
            return Ray(origin, (mouseWPos - origin).normalized());
        }
        const float orthoDepth = _camera && _camera->camera() ?
            _camera->camera()->farClip() - _camera->camera()->nearClip() : 0.0f;
        const Vector3 forward = cameraForward();
        return Ray(mouseWPos - forward * orthoDepth, forward);
    }

    Plane TransformGizmo::createPlane(const GizmoAxis axis, const bool isFacing, const bool isLine) const
    {
        const Vector3 facing = facingDir();
        Vector3 normal(0.0f);

        if (isFacing) {
            // set plane normal to face camera
            normal = -cameraForward();
        } else {
            // set plane normal based on axis
            normal = withGizmoComponent(normal, gizmoAxisIndex(axis), 1.0f);
            normal = _rootStartRot * normal;

            if (isLine) {
                // set plane normal to face camera but keep normal perpendicular to axis
                const Vector3 v2 = normal.cross(facing).normalized();
                normal = v2.cross(normal).normalized();
            }
        }

        Plane plane;
        plane.setFromPointNormal(_rootStartPos, normal);
        return plane;
    }

    Vector3 TransformGizmo::dirFromAxis(const GizmoAxis axis) const
    {
        if (axis == GizmoAxis::F) {
            return -cameraForward();
        }
        return withGizmoComponent(Vector3(0.0f), gizmoAxisIndex(axis), 1.0f);
    }

    Vector3 TransformGizmo::projectToAxis(const Vector3& point, const GizmoAxis axis)
    {
        // project onto the axis, then zero the other components (upstream's float fix)
        const int index = gizmoAxisIndex(axis);
        if (index < 0) {
            return Vector3(0.0f);
        }
        const Vector3 unit = withGizmoComponent(Vector3(0.0f), index, 1.0f);
        const Vector3 projected = unit * unit.dot(point);
        return withGizmoComponent(Vector3(0.0f), index, projected[index]);
    }

    Vector3 TransformGizmo::screenToPoint(const float x, const float y)
    {
        if (!_camera) {
            return _point;
        }
        const Vector3 mouseWPos = _camera->screenToWorld(x, y, 1.0f);
        const Ray ray = createRay(mouseWPos);
        const Plane plane = createPlane(_selectedAxis, false, false);
        Vector3 hit;
        if (plane.intersectsRay(ray, &hit)) {
            _point = hit;
        }
        return _point;
    }

    void TransformGizmo::drawGuideLines(const Vector3& pos, const Quaternion& rot, const GizmoAxis activeAxis,
        const bool activeIsPlane)
    {
        for (const GizmoAxis axis : AXES) {
            if (activeAxis == GizmoAxis::XYZ) {
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

    void TransformGizmo::drawSpanLine(const Vector3& pos, const Quaternion& rot, const GizmoAxis axis)
    {
        if (!_camera || !_camera->camera()) {
            return;
        }
        const Vector3 dir = dirFromAxis(axis);
        const Color* base = axis == GizmoAxis::X ? &_theme.guideBase.x :
            axis == GizmoAxis::Y ? &_theme.guideBase.y : &_theme.guideBase.z;
        const Vector3 from = rot * (dir * (_camera->camera()->farClip() - _camera->camera()->nearClip())) + pos;
        const Vector3 to = rot * (dir * -(_camera->camera()->farClip() - _camera->camera()->nearClip())) + pos;
        _frameGuideLines.emplace_back(from, to);
        if (_theme.guideOcclusion < 1.0f) {
            Color occluded = *base;
            occluded.a *= (1.0f - _theme.guideOcclusion);
            _occludedLines.push_back({from, to, occluded});
        }
        if (base->a != 0.0f) {
            _baseLines.push_back({from, to, *base});
        }
    }

    void TransformGizmo::flushGuideLines()
    {
        if (!_engine || !_engine->graphicsDevice() || !_layer) {
            return;
        }
        if (!_baseRenderer && _baseLines.empty() && _occludedLines.empty()) {
            return;
        }
        if (!_baseRenderer) {
            // created on first use, so their render components follow the shapes' in the
            // gizmo layer's collection order, as upstream's immediate lines do
            _baseRenderer = std::make_unique<WideLineRenderer>(_engine, _engine->graphicsDevice());
            _baseRenderer->setLayers({LAYERID_IMMEDIATE});
            _occludedRenderer = std::make_unique<WideLineRenderer>(_engine, _engine->graphicsDevice());
            _occludedRenderer->setLayers({_layer->id()});
            _occludedRenderer->setBlend(true);
            _occludedRenderer->setDepthTest(false);
        }

        const auto [width, height] = _engine->graphicsDevice()->size();
        _baseRenderer->setScreenSize(static_cast<float>(width), static_cast<float>(height));
        _occludedRenderer->setScreenSize(static_cast<float>(width), static_cast<float>(height));

        // A wide line is expanded in screen space, so a point behind the camera would fold
        // the line back across the screen. Clip each to just in front of the near plane.
        const Vector3 camPos = cameraPosition();
        const Vector3 forward = cameraForward();
        const float nearClip = _camera && _camera->camera() ? _camera->camera()->nearClip() * 1.01f : 0.0f;
        const auto clip = [&](Vector3& a, Vector3& b) {
            const float da = (a - camPos).dot(forward) - nearClip;
            const float db = (b - camPos).dot(forward) - nearClip;
            if (da < 0.0f && db < 0.0f) {
                return false;
            }
            if (da < 0.0f) {
                a = a + (b - a) * (da / (da - db));
            } else if (db < 0.0f) {
                b = b + (a - b) * (db / (db - da));
            }
            return true;
        };

        const auto sync = [&](WideLineRenderer& renderer, std::array<WideLine, 3>& wideLines, size_t& shown,
            const std::vector<GuideLine>& lines) {
            size_t count = 0;
            for (const GuideLine& line : lines) {
                if (count >= wideLines.size()) {
                    break;
                }
                Vector3 a = line.from;
                Vector3 b = line.to;
                if (!clip(a, b)) {
                    continue;
                }
                WideLine& wide = wideLines[count];
                wide.setPoints({a, b}, line.color, 1.0f);
                wide.setOpacity(line.color.a);
                if (count >= shown) {
                    renderer.add(&wide);
                }
                ++count;
            }
            for (size_t i = count; i < shown; ++i) {
                renderer.remove(&wideLines[i]);
            }
            shown = count;
            renderer.update();
        };
        sync(*_baseRenderer, _baseWideLines, _baseShown, _baseLines);
        sync(*_occludedRenderer, _occludedWideLines, _occludedShown, _occludedLines);
    }

    void TransformGizmo::createTransform()
    {
        for (auto& [key, s] : _shapes) {
            if (auto owned = s->takeEntity()) {
                _root->addChild(std::move(owned));
            }
            _intersectShapes.push_back(s.get());
        }
    }

    void TransformGizmo::enableShape(const GizmoAxis shapeAxis, const bool enabled)
    {
        if (Shape* s = shape(shapeAxis)) {
            s->setDisabled(!enabled);
        }
    }

    bool TransformGizmo::isShapeEnabled(const GizmoAxis shapeAxis) const
    {
        const Shape* s = shape(shapeAxis);
        return s && !s->disabled();
    }

    void TransformGizmo::setTheme(const GizmoThemePartial& partial)
    {
        // shape
        copyIfSet(_theme.shapeBase.x, partial.shapeBase.x);
        copyIfSet(_theme.shapeBase.y, partial.shapeBase.y);
        copyIfSet(_theme.shapeBase.z, partial.shapeBase.z);
        copyIfSet(_theme.shapeBase.xyz, partial.shapeBase.xyz);
        copyIfSet(_theme.shapeBase.f, partial.shapeBase.f);
        copyIfSet(_theme.shapeHover.x, partial.shapeHover.x);
        copyIfSet(_theme.shapeHover.y, partial.shapeHover.y);
        copyIfSet(_theme.shapeHover.z, partial.shapeHover.z);
        copyIfSet(_theme.shapeHover.xyz, partial.shapeHover.xyz);
        copyIfSet(_theme.shapeHover.f, partial.shapeHover.f);

        // guide
        copyIfSet(_theme.guideBase.x, partial.guideBase.x);
        copyIfSet(_theme.guideBase.y, partial.guideBase.y);
        copyIfSet(_theme.guideBase.z, partial.guideBase.z);
        if (partial.guideOcclusion) {
            _theme.guideOcclusion = std::clamp(*partial.guideOcclusion, 0.0f, 1.0f);
        }

        // disabled
        copyIfSet(_theme.disabled, partial.disabled);

        // update shapes (upstream: every shape takes the hover state of ANY hovered axis)
        for (auto& [key, s] : _shapes) {
            s->hover(_hoverAxis != GizmoAxis::None);
        }
    }

    void TransformGizmo::prerender()
    {
        Gizmo::prerender();

        _baseLines.clear();
        _occludedLines.clear();
        _frameGuideLines.clear();

        if (enabled()) {
            const Vector3 gizmoPos = _root->localPosition();
            const Quaternion gizmoRot = _root->rotation();
            const GizmoAxis activeAxis = _hoverAxis != GizmoAxis::None ? _hoverAxis : _selectedAxis;
            const bool activeIsPlane = _hoverIsPlane || _selectedIsPlane;
            drawGuideLines(gizmoPos, gizmoRot, activeAxis, activeIsPlane);
        }

        flushGuideLines();
    }

    void TransformGizmo::releaseGuideLines()
    {
        _baseRenderer.reset();
        _occludedRenderer.reset();
        _baseShown = 0;
        _occludedShown = 0;
    }

    void TransformGizmo::destroy()
    {
        releaseGuideLines();
        Gizmo::destroy();
    }
}
