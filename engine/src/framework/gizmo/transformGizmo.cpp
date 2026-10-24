// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
#include "transformGizmo.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

#include "core/math/defines.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/gizmo/overlay/overlayRenderer.h"
#include "framework/gizmo/shape/shapes.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/camera.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/meshInstance.h"
#include "scene/scene.h"

namespace visutwin::canvas
{
    namespace
    {
        // How far from end-on (an axis) or edge-on (a plane) a shape is still drawn.
        constexpr float GLANCE_EPSILON = 0.01f;
        const float GLANCE_ANGLE = std::acos(1.0f - GLANCE_EPSILON);

        bool isSingleAxis(const GizmoAxis key)
        {
            return key == GizmoAxis::X || key == GizmoAxis::Y || key == GizmoAxis::Z || key == GizmoAxis::F;
        }
    }

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
        : DragGizmo(camera, std::move(layer), name)
    {
        // guides sit on an unrotated root in the Immediate layer, whose depth lets the scene hide
        // them, or follow the gizmo layer without one
        auto guideRoot = std::make_unique<Entity>();
        _guideRoot = guideRoot.get();
        _guideRoot->setEngine(_engine);
        _guideRoot->setName("guides");
        if (_engine && _engine->root()) {
            _engine->root()->addChild(std::move(guideRoot));
        } else {
            guideRoot.release();   // owned by `_guideRoot`, freed in destroy()
        }
        std::shared_ptr<Layer> guideLayer;
        if (_engine && _engine->scene() && _engine->scene()->layers()) {
            guideLayer = _engine->scene()->layers()->getLayerById(LAYERID_IMMEDIATE);
        }
        _guides = std::make_unique<OverlayRenderer>(this,
            OverlayRendererTheme{&_guideBase, &_guideOccluded, &_guideFill, nullptr}, _guideRoot, guideLayer);
        _guides->lineThickness = 1.0f;

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

        on(EVENT_TRANSFORMEND, [this]() {
            hideReadout();
        });
    }

    void TransformGizmo::setReadout(const bool value)
    {
        _readoutEnabled = value;
        if (!value) {
            hideReadout();
        }
    }

    float TransformGizmo::guideThickness() const
    {
        return _guides ? _guides->lineThickness : 1.0f;
    }

    void TransformGizmo::setGuideThickness(const float value)
    {
        if (_guides) {
            _guides->lineThickness = value;
        }
    }

    std::string TransformGizmo::readoutLabel(const GizmoAxis axis) const
    {
        const Color* color = themeColor(_theme.shapeBase, axis);
        const char* letter = axis == GizmoAxis::X ? "X" : axis == GizmoAxis::Y ? "Y" : axis == GizmoAxis::Z ? "Z" : "";
        if (!color) {
            return letter;
        }
        return labelMarkup(letter, *color);
    }

    std::string TransformGizmo::readoutDelta(const float value, const int digits)
    {
        char magnitude[32];
        std::snprintf(magnitude, sizeof(magnitude), "%.*f", digits, std::abs(value));
        const bool negative = value < 0.0f && std::strtod(magnitude, nullptr) != 0.0;
        return std::string(negative ? "-" : "+") + magnitude;
    }

    std::string TransformGizmo::readoutAxes(const Vector3& values, const bool factor) const
    {
        const GizmoAxis axis = _selectedAxis;
        std::string text;
        for (size_t i = 0; i < AXES.size(); ++i) {
            const GizmoAxis a = AXES[i];
            // A plane moves along the other two axes; the centre along all three.
            const bool shown = axis == GizmoAxis::XYZ ||
                (_selectedIsPlane ? gizmoAxisIndex(axis) != static_cast<int>(i) : a == axis);
            if (!shown) {
                continue;
            }
            const float value = values[static_cast<int>(i)];
            std::string part;
            if (factor) {
                char number[32];
                std::snprintf(number, sizeof(number), "\u00d7%.2f", value);
                part = number;
            } else {
                part = readoutDelta(value, 2);
            }
            if (!text.empty()) {
                text += "  ";
            }
            text += readoutLabel(a) + " " + part;
        }
        return text;
    }

    void TransformGizmo::showReadout(const std::string& markup)
    {
        if (!_readoutEnabled || !_camera) {
            return;
        }
        BoundingBox bounds;
        bool found = false;
        for (const auto& [key, shape] : _shapes) {
            if (!shape || !shape->entity() || !shape->entity()->enabled()) {
                continue;
            }
            for (auto* instance : shape->meshInstances()) {
                if (!instance || !instance->visible()) {
                    continue;
                }
                if (found) {
                    bounds.add(instance->aabb());
                } else {
                    bounds = instance->aabb();
                    found = true;
                }
            }
        }
        // with every shape hidden while dragging, sit below the gizmo centre
        if (!found) {
            bounds = BoundingBox(rootPosition(), Vector3(0.0f));
        }

        const Vector3 center = bounds.center();
        const Vector3 half = bounds.halfExtents();
        float left = std::numeric_limits<float>::max();
        float right = std::numeric_limits<float>::lowest();
        float bottom = std::numeric_limits<float>::lowest();
        for (int i = 0; i < 8; ++i) {
            const Vector3 corner(center.getX() + ((i & 1) ? half.getX() : -half.getX()),
                center.getY() + ((i & 2) ? half.getY() : -half.getY()),
                center.getZ() + ((i & 4) ? half.getZ() : -half.getZ()));
            const Vector3 screen = _camera->worldToScreen(corner);
            left = std::min(left, screen.getX());
            right = std::max(right, screen.getX());
            bottom = std::max(bottom, screen.getY());
        }
        showReadoutAt(markup, 0.5f * (left + right), bottom);
    }

    void TransformGizmo::showReadoutAt(const std::string& markup, const float x, const float y)
    {
        if (!_readoutEnabled) {
            return;
        }
        // A small gap below the point.
        constexpr float kGap = 6.0f;
        _readout.visible = true;
        _readout.markup = markup;
        _readout.x = x;
        _readout.y = y + kGap;
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
                // face the camera but stay perpendicular to the axis
                Vector3 lineNormalOut;
                lineNormal(normal, facing, lineNormalOut);
                normal = lineNormalOut;
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
        // project onto the axis, then zero the other components
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
        _guides->line(from, to, 1.0f, base->a, base);
    }

    bool TransformGizmo::glance(Shape* shape, const float cos)
    {
        const bool visible = 1.0f - cos > GLANCE_EPSILON;
        const float range = _fadeAngle * DEG_TO_RAD - GLANCE_ANGLE;
        shape->setFade(visible && range > 0.0f
            ? std::clamp((std::acos(std::min(cos, 1.0f)) - GLANCE_ANGLE) / range, 0.0f, 1.0f) : 1.0f);
        if (shape->entity()->enabledLocal() == visible) {
            return false;
        }
        shape->entity()->setEnabled(visible);
        return true;
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

        // update shapes: each keeps its own hover state
        for (auto& [key, s] : _shapes) {
            s->hover(std::find(_hovering.begin(), _hovering.end(), key) != _hovering.end());
        }
    }

    void TransformGizmo::prerender()
    {
        Gizmo::prerender();

        _frameGuideLines.clear();
        if (!_guides) {
            return;
        }
        if (!enabled()) {
            _guides->clear();
            return;
        }

        _guideOccluded.a = 1.0f - _theme.guideOcclusion;
        _guides->begin();
        const Vector3 gizmoPos = _root->localPosition();
        const Quaternion gizmoRot = _root->rotation();
        const GizmoAxis activeAxis = _hoverAxis != GizmoAxis::None ? _hoverAxis : _selectedAxis;
        const bool activeIsPlane = _hoverIsPlane || _selectedIsPlane;
        drawGuideLines(gizmoPos, gizmoRot, activeAxis, activeIsPlane);
        _guides->end();
    }

    void TransformGizmo::destroy()
    {
        if (_guides) {
            _guides->destroy();
            _guides.reset();
        }
        if (_guideRoot) {
            if (_guideRoot->parent()) {
                auto owned = _guideRoot->remove();
            } else {
                delete _guideRoot;
            }
            _guideRoot = nullptr;
        }
        Gizmo::destroy();
    }

    void TransformGizmo::updateAxisAndPlaneShapes(const std::array<Shape*, 3>& axes,
        const std::array<PlaneShape*, 3>& planes, const bool flipPlanes)
    {
        const Vector3 dir = cameraDir();
        const Vector3 right = rootRight();
        const Vector3 up = rootUp();
        const Vector3 forward = rootForward();

        // axes
        bool changed = glance(axes[0], std::abs(dir.dot(right)));
        changed = glance(axes[1], std::abs(dir.dot(up))) || changed;
        changed = glance(axes[2], std::abs(dir.dot(forward))) || changed;

        // planes
        const auto setFlipped = [&changed](PlaneShape* plane, const Vector3& flipped) {
            if (!gizmoVectorEquals(plane->flipped(), flipped)) {
                plane->setFlipped(flipped);
                changed = true;
            }
        };
        const auto b = [](const bool v) { return v ? 1.0f : 0.0f; };

        Vector3 v1 = dir.cross(right);
        changed = glance(planes[0], v1.length()) || changed;
        setFlipped(planes[0], flipPlanes ? Vector3(0.0f, b(v1.dot(forward) < 0.0f), b(v1.dot(up) < 0.0f)) : Vector3(0.0f));

        v1 = dir.cross(forward);
        changed = glance(planes[2], v1.length()) || changed;
        setFlipped(planes[2], flipPlanes ? Vector3(b(v1.dot(up) < 0.0f), b(v1.dot(right) > 0.0f), 0.0f) : Vector3(0.0f));

        v1 = dir.cross(up);
        changed = glance(planes[1], v1.length()) || changed;
        setFlipped(planes[1], flipPlanes ? Vector3(b(v1.dot(forward) > 0.0f), 0.0f, b(v1.dot(right) > 0.0f)) : Vector3(0.0f));

        if (changed) {
            _renderUpdate = true;
        }
    }

    void TransformGizmo::applyAxisDragVisibility(const bool state)
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
}
