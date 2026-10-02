// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
#include "gizmo.h"

#include <algorithm>
#include <cmath>

#include <SDL3/SDL_mouse.h>

#include "core/math/defines.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/gizmo/shape/shape.h"
#include "platform/input/inputConstants.h"
#include "platform/input/mouse.h"
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
        // gizmo constants
        constexpr float MIN_SCALE = 1e-4f;
        constexpr float PERS_SCALE_RATIO = 0.3f;
        constexpr float ORTHO_SCALE_RATIO = 0.32f;
        constexpr float UPDATE_EPSILON = 1e-6f;
        constexpr float DIST_EPSILON = 1e-4f;

        bool equalsApprox(const Vector3& a, const Vector3& b, const float epsilon)
        {
            return std::abs(a.getX() - b.getX()) < epsilon && std::abs(a.getY() - b.getY()) < epsilon &&
                std::abs(a.getZ() - b.getZ()) < epsilon;
        }

        bool equalsApprox(const Quaternion& a, const Quaternion& b, const float epsilon)
        {
            return std::abs(a.getX() - b.getX()) < epsilon && std::abs(a.getY() - b.getY()) < epsilon &&
                std::abs(a.getZ() - b.getZ()) < epsilon && std::abs(a.getW() - b.getW()) < epsilon;
        }

        int buttonIndex(const MouseButton button)
        {
            switch (button) {
                case MouseButton::Left: return 0;
                case MouseButton::Middle: return 1;
                case MouseButton::Right: return 2;
                default: return -1;
            }
        }

        // The layers a camera with an empty list renders (it renders all of them), spelled
        // out so a gizmo layer can be added to them.
        std::vector<int> explicitCameraLayers(const CameraComponent* camera)
        {
            std::vector<int> ids = camera->layers();
            if (ids.empty()) {
                ids = {LAYERID_WORLD, LAYERID_DEPTH, LAYERID_SKYBOX, LAYERID_UI, LAYERID_IMMEDIATE};
            }
            return ids;
        }

        int unusedLayerId(const LayerComposition& layers)
        {
            int id = 1000;
            while (layers.getLayerById(id)) {
                ++id;
            }
            return id;
        }
    }

    const char* gizmoAxisName(const GizmoAxis axis)
    {
        switch (axis) {
            case GizmoAxis::X: return "x";
            case GizmoAxis::Y: return "y";
            case GizmoAxis::Z: return "z";
            case GizmoAxis::YZ: return "yz";
            case GizmoAxis::XZ: return "xz";
            case GizmoAxis::XY: return "xy";
            case GizmoAxis::XYZ: return "xyz";
            case GizmoAxis::F: return "f";
            default: return "";
        }
    }

    float gizmoViewportScale(const bool perspective, const float fovDegrees, const float forwardDistance,
        const float orthoHeight, const float size)
    {
        float scale;
        if (perspective) {
            scale = std::tan(0.5f * fovDegrees * DEG_TO_RAD) * forwardDistance * PERS_SCALE_RATIO;
        } else {
            scale = orthoHeight * ORTHO_SCALE_RATIO;
        }
        return std::max(scale * size, MIN_SCALE);
    }

    Vector3 gizmoEulerAngles(const Quaternion& q)
    {
        const float qx = q.getX();
        const float qy = q.getY();
        const float qz = q.getZ();
        const float qw = q.getW();
        const float a2 = 2.0f * (qw * qy - qx * qz);
        float x;
        float y;
        float z;
        if (a2 <= -0.99999f) {
            x = 2.0f * std::atan2(qx, qw);
            y = -PI / 2.0f;
            z = 0.0f;
        } else if (a2 >= 0.99999f) {
            x = 2.0f * std::atan2(qx, qw);
            y = PI / 2.0f;
            z = 0.0f;
        } else {
            x = std::atan2(2.0f * (qw * qx + qy * qz), 1.0f - 2.0f * (qx * qx + qy * qy));
            y = std::asin(a2);
            z = std::atan2(2.0f * (qw * qz + qx * qy), 1.0f - 2.0f * (qy * qy + qz * qz));
        }
        return Vector3(x, y, z) * RAD_TO_DEG;
    }

    std::shared_ptr<Layer> Gizmo::createLayer(Engine* engine, const std::string& layerName, const int layerIndex)
    {
        if (!engine || !engine->scene() || !engine->scene()->layers()) {
            return nullptr;
        }
        const auto& layers = engine->scene()->layers();
        auto layer = std::make_shared<Layer>(layerName, unusedLayerId(*layers));
        layer->setClearDepthBuffer(true);
        layer->setOpaqueSortMode(SortMode::SORTMODE_NONE);
        layer->setTransparentSortMode(SortMode::SORTMODE_NONE);
        layers->insert(layer, layerIndex);
        return layer;
    }

    Gizmo::Gizmo(CameraComponent* camera, std::shared_ptr<Layer> layer, const std::string& name)
        : _camera(camera), _layer(std::move(layer))
    {
        _engine = _camera && _camera->entity() ? _camera->entity()->engine() : nullptr;

        addLayerToCamera();
        if (_camera && _camera->entity()) {
            _cameraDestroyed.watch(_camera->entity(), [this] { _camera = nullptr; });
        }

        auto root = std::make_unique<Entity>();
        _root = root.get();
        _root->setEngine(_engine);
        _root->setName(name);
        if (_engine && _engine->root()) {
            _engine->root()->addChild(std::move(root));
        } else {
            root.release();   // owned by `_root`, freed in destroy()
        }
        _root->setEnabled(false);

        updateScale();

        if (_engine) {
            if (Mouse* mouse = _engine->mouse()) {
                _handles.push_back(mouse->on("mousedown", [this](const MouseEvent& e) {
                    if (const int button = buttonIndex(e.button); button >= 0) {
                        pointerDown(e.x, e.y, button);
                    }
                }));
                _handles.push_back(mouse->on("mousemove", [this](const MouseEvent& e) {
                    pointerMove(e.x, e.y);
                }));
                _handles.push_back(mouse->on("mouseup", [this](const MouseEvent& e) {
                    if (const int button = buttonIndex(e.button); button >= 0) {
                        pointerUp(e.x, e.y, button);
                    }
                }));
            }
            _handles.push_back(_engine->on("prerender", [this]() { prerender(); }));
            _handles.push_back(_engine->on("update", [this]() { update(); }));
            _handles.push_back(_engine->on("destroy", [this]() { destroy(); }));
        }
    }

    Gizmo::~Gizmo()
    {
        Gizmo::destroy();
    }

    void Gizmo::addLayerToCamera()
    {
        if (!_camera || !_layer) {
            return;
        }
        std::vector<int> ids = explicitCameraLayers(_camera);
        ids.push_back(_layer->id());
        _camera->setLayers(ids);
    }

    void Gizmo::removeLayerFromCamera()
    {
        if (!_camera || !_layer) {
            return;
        }
        std::vector<int> ids = explicitCameraLayers(_camera);
        std::erase(ids, _layer->id());
        _camera->setLayers(ids);
    }

    bool Gizmo::enabled() const
    {
        return _root && _root->enabledLocal();
    }

    void Gizmo::setEnabled(const bool state)
    {
        if (!_root) {
            return;
        }
        const float cameraDist = _camera ? _root->localPosition().distance(cameraPosition()) : 0.0f;
        const bool enabled = state ? !_nodes.empty() && cameraDist > DIST_EPSILON : false;
        if (enabled != _root->enabledLocal()) {
            _root->setEnabled(enabled);
            _renderUpdate = true;
        }
    }

    void Gizmo::setLayer(const std::shared_ptr<Layer>& layer)
    {
        if (_layer == layer) {
            return;
        }
        removeLayerFromCamera();
        _layer = layer;
        addLayerToCamera();
        setEnabled(true);
    }

    void Gizmo::setCamera(CameraComponent* camera)
    {
        if (_camera == camera) {
            return;
        }
        removeLayerFromCamera();
        _cameraDestroyed.reset();
        _camera = camera;
        addLayerToCamera();
        if (_camera && _camera->entity()) {
            _cameraDestroyed.watch(_camera->entity(), [this] { _camera = nullptr; });
        }
        setEnabled(true);
    }

    void Gizmo::setCoordSpace(const GizmoSpace value)
    {
        _coordSpace = value;
        updateRotation();
    }

    void Gizmo::setSize(const float value)
    {
        _size = value;
        updateScale();
    }

    Vector3 Gizmo::cameraPosition() const
    {
        return _camera && _camera->entity() ? _camera->entity()->position() : Vector3(0.0f);
    }

    Quaternion Gizmo::cameraRotation() const
    {
        return _camera && _camera->entity() ? _camera->entity()->rotation() : Quaternion();
    }

    Vector3 Gizmo::cameraForward() const
    {
        if (!_camera || !_camera->entity()) {
            return Vector3(0.0f, 0.0f, -1.0f);
        }
        return (-Vector3(_camera->entity()->worldTransform().getColumn(2))).normalized();
    }

    Vector3 Gizmo::cameraRightAxis() const
    {
        if (!_camera || !_camera->entity()) {
            return Vector3(1.0f, 0.0f, 0.0f);
        }
        return Vector3(_camera->entity()->worldTransform().getColumn(0)).normalized();
    }

    Vector3 Gizmo::cameraUpAxis() const
    {
        if (!_camera || !_camera->entity()) {
            return Vector3(0.0f, 1.0f, 0.0f);
        }
        return Vector3(_camera->entity()->worldTransform().getColumn(1)).normalized();
    }

    Vector3 Gizmo::rootPosition() const
    {
        return _root ? _root->localPosition() : Vector3(0.0f);
    }

    Quaternion Gizmo::rootRotation() const
    {
        return _root ? _root->rotation() : Quaternion();
    }

    Vector3 Gizmo::rootRight() const
    {
        return Vector3(_root->worldTransform().getColumn(0)).normalized();
    }

    Vector3 Gizmo::rootUp() const
    {
        return Vector3(_root->worldTransform().getColumn(1)).normalized();
    }

    Vector3 Gizmo::rootForward() const
    {
        return (-Vector3(_root->worldTransform().getColumn(2))).normalized();
    }

    Vector3 Gizmo::facingDir() const
    {
        if (_camera && _camera->camera() && _camera->camera()->projection() == ProjectionType::Perspective) {
            return (cameraPosition() - rootPosition()).normalized();
        }
        return -cameraForward();
    }

    Vector3 Gizmo::cameraDir() const
    {
        return (cameraPosition() - rootPosition()).normalized();
    }

    void Gizmo::pointerDown(const float x, const float y, const int button)
    {
        if (!enabled() || (_engine && _engine->mouse() && _engine->mouse()->relativeMode())) {
            return;
        }
        if (button < 0 || button > 2 || !_mouseButtons[static_cast<size_t>(button)]) {
            return;
        }
        MeshInstance* selection = getSelection(x, y);

        // Capture the pointer during the drag, so a drag that
        // leaves the window keeps reporting.
        if (!_captured) {
            _captured = SDL_CaptureMouse(true);
        }

        fire(EVENT_POINTERDOWN, x, y, selection);
    }

    void Gizmo::pointerMove(const float x, const float y)
    {
        if (!enabled() || (_engine && _engine->mouse() && _engine->mouse()->relativeMode())) {
            return;
        }
        MeshInstance* selection = getSelection(x, y);
        fire(EVENT_POINTERMOVE, x, y, selection);
    }

    void Gizmo::pointerUp(const float x, const float y, const int button)
    {
        if (!enabled() || (_engine && _engine->mouse() && _engine->mouse()->relativeMode())) {
            return;
        }
        if (button < 0 || button > 2 || !_mouseButtons[static_cast<size_t>(button)]) {
            return;
        }
        MeshInstance* selection = getSelection(x, y);

        if (_captured) {
            SDL_CaptureMouse(false);
            _captured = false;
        }

        fire(EVENT_POINTERUP, x, y, selection);
    }

    void Gizmo::updatePosition()
    {
        if (!_root) {
            return;
        }
        Vector3 position(0.0f);
        if (_coordSpace == GizmoSpace::Local) {
            if (!_nodes.empty()) {
                position = _nodes.back()->position();
            }
        } else {
            for (GraphNode* node : _nodes) {
                position = position + node->position();
            }
            position = position * (1.0f / static_cast<float>(std::max<size_t>(_nodes.size(), 1)));
        }

        if (equalsApprox(position, _root->localPosition(), UPDATE_EPSILON)) {
            return;
        }

        _root->setLocalPosition(position);
        fire(EVENT_POSITIONUPDATE, position);

        _renderUpdate = true;
    }

    void Gizmo::updateRotation()
    {
        if (!_root) {
            return;
        }
        Quaternion rotation;
        if (_coordSpace == GizmoSpace::Local && !_nodes.empty()) {
            rotation = _nodes.back()->rotation();
        }

        if (equalsApprox(rotation, _root->rotation(), UPDATE_EPSILON)) {
            return;
        }

        _root->setRotation(rotation);
        fire(EVENT_ROTATIONUPDATE, gizmoEulerAngles(rotation));

        _renderUpdate = true;
    }

    void Gizmo::updateScale()
    {
        if (!_root || !_camera || !_camera->camera()) {
            return;
        }
        const Camera* camera = _camera->camera();
        const bool perspective = camera->projection() == ProjectionType::Perspective;
        const float dist = perspective ? (_root->localPosition() - cameraPosition()).dot(cameraForward()) : 0.0f;
        _scale = gizmoViewportScale(perspective, camera->fov(), dist, camera->orthoHeight(), _size);

        if (std::abs(_scale - _root->localScale().getX()) < UPDATE_EPSILON) {
            return;
        }

        _root->setLocalScale(_scale, _scale, _scale);
        fire(EVENT_SCALEUPDATE, _scale);

        _renderUpdate = true;
    }

    MeshInstance* Gizmo::getSelection(const float x, const float y) const
    {
        if (!_camera || !_camera->camera()) {
            return nullptr;
        }
        const Camera* camera = _camera->camera();
        const Vector3 start = _camera->screenToWorld(x, y, 0.0f);
        const Vector3 end = _camera->screenToWorld(x, y, camera->farClip() - camera->nearClip());
        const Vector3 dir = (end - start).normalized();

        struct Hit
        {
            float dist;
            const Shape* shape;
            int priority;
        };
        std::vector<Hit> selection;
        for (const Shape* shape : _intersectShapes) {
            if (shape->disabled() || !shape->entity()->enabled()) {
                continue;
            }

            const Matrix4& parentTM = shape->entity()->worldTransform();
            for (const TriData* triData : shape->triData()) {
                float dist = 0.0f;
                if (triData->intersect(parentTM, start, dir, dist)) {
                    selection.push_back({dist, shape, triData->priority()});
                }
            }
        }

        if (selection.empty()) {
            return nullptr;
        }
        // The hits order by this comparator and the first is taken: two prioritised
        // hits order by priority, any other pair by distance. It is not a strict weak order
        // (a sort may not be handed it), so the first is found by a scan that replaces the
        // best only on "less".
        const auto less = [](const Hit& a, const Hit& b) {
            if (a.priority != 0 && b.priority != 0) {
                return b.priority < a.priority;
            }
            return a.dist < b.dist;
        };
        size_t best = 0;
        for (size_t i = 1; i < selection.size(); ++i) {
            if (less(selection[i], selection[best])) {
                best = i;
            }
        }
        const auto& instances = selection[best].shape->meshInstances();
        return instances.empty() ? nullptr : instances.front();
    }

    void Gizmo::watchNodes()
    {
        unwatchNodes();
        for (GraphNode* node : _nodes) {
            if (auto* entity = dynamic_cast<Entity*>(node)) {
                _nodeHandles.push_back(entity->on("destroy", [this, node]() { onNodeDestroyed(node); }));
            }
        }
    }

    void Gizmo::unwatchNodes()
    {
        for (const auto& handle : _nodeHandles) {
            handle->off();
        }
        _nodeHandles.clear();
    }

    void Gizmo::onNodeDestroyed(GraphNode* node)
    {
        // DEVIATION: upstream keeps a destroyed node in `nodes` (its garbage collector keeps
        // the object alive); here the pointer would dangle, so the node leaves the list and
        // the gizmo detaches once none is left.
        std::erase(_nodes, node);
        if (_nodes.empty()) {
            detach();
        }
    }

    void Gizmo::attach(const std::vector<GraphNode*>& nodes)
    {
        if (nodes.empty()) {
            return;
        }
        _nodes = nodes;
        std::erase(_nodes, nullptr);
        watchNodes();

        updatePosition();
        updateRotation();
        updateScale();

        fire(EVENT_NODESATTACH);

        setEnabled(true);
    }

    void Gizmo::attach(GraphNode* node)
    {
        if (node) {
            attach(std::vector<GraphNode*>{node});
        }
    }

    void Gizmo::detach()
    {
        setEnabled(false);

        fire(EVENT_NODESDETACH);

        unwatchNodes();
        _nodes.clear();
    }

    void Gizmo::update()
    {
        if (_renderUpdate) {
            _renderUpdate = false;
            fire(EVENT_RENDERUPDATE);
        }

        if (!enabled()) {
            return;
        }

        updatePosition();
        updateRotation();
        updateScale();
    }

    void Gizmo::destroy()
    {
        if (_destroyed) {
            return;
        }
        _destroyed = true;

        detach();

        for (const auto& handle : _handles) {
            handle->off();
        }
        _handles.clear();
        _cameraDestroyed.reset();
        if (_captured) {
            SDL_CaptureMouse(false);
            _captured = false;
        }

        removeLayerFromCamera();
        _intersectShapes.clear();

        if (_root) {
            if (_root->parent()) {
                auto owned = _root->remove();
            } else {
                delete _root;
            }
            _root = nullptr;
        }
        _engine = nullptr;
    }
}
