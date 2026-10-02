// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Upstream extras/gizmo/gizmo.js: the base of every gizmo.
//
// A gizmo is an interactive widget drawn over the scene in its own Layer —
// `createLayer` makes one that clears depth and keeps collection order, and the
// constructor adds it to the camera. It acts on the GraphNodes it is attached to, keeps
// a constant apparent size as the camera moves (`size` scales it), follows the 'world'
// or 'local' axes, and selects its shapes by intersecting the pointer ray with their
// triangles (TriData). It updates itself from the engine's `update` and `prerender`
// events, so it needs no per-frame call, and reads the pointer from the engine's mouse
// device (`mousedown` / `mousemove` / `mouseup`, window points), so it needs no event
// forwarding either.
//
// Events, as upstream: `pointer:down`, `pointer:move`, `pointer:up` (float x, float y,
// MeshInstance* — null off the gizmo), `position:update` (Vector3), `rotation:update`
// (Vector3 Euler angles), `scale:update` (float), `nodes:attach`, `nodes:detach` and
// `render:update`.
//
#pragma once

#include <array>
#include <climits>
#include <memory>
#include <string>
#include <vector>

#include "core/eventHandler.h"
#include "core/math/quaternion.h"
#include "core/math/vector3.h"
#include "framework/gizmo/gizmoConstants.h"

namespace visutwin::canvas
{
    class CameraComponent;
    class Engine;
    class Entity;
    class GraphNode;
    class Layer;
    class MeshInstance;
    class Shape;

    /// Upstream Gizmo._updateScale: the world size that keeps a gizmo the same size on
    /// screen. Perspective: tan(fov / 2) x the distance along the camera's forward x 0.3;
    /// orthographic: orthoHeight x 0.32; then times `size`, never below 1e-4.
    float gizmoViewportScale(bool perspective, float fovDegrees, float forwardDistance, float orthoHeight, float size);

    /// Upstream Quat.getEulerAngles, in degrees.
    Vector3 gizmoEulerAngles(const Quaternion& q);

    class Gizmo : public EventHandler
    {
    public:
        static constexpr const char* EVENT_POINTERDOWN = "pointer:down";
        static constexpr const char* EVENT_POINTERMOVE = "pointer:move";
        static constexpr const char* EVENT_POINTERUP = "pointer:up";
        static constexpr const char* EVENT_POSITIONUPDATE = "position:update";
        static constexpr const char* EVENT_ROTATIONUPDATE = "rotation:update";
        static constexpr const char* EVENT_SCALEUPDATE = "scale:update";
        static constexpr const char* EVENT_NODESATTACH = "nodes:attach";
        static constexpr const char* EVENT_NODESDETACH = "nodes:detach";
        static constexpr const char* EVENT_RENDERUPDATE = "render:update";

        /// Upstream Gizmo.createLayer: a layer that clears depth and sorts nothing,
        /// inserted into the scene's composition at `layerIndex` (the end by default).
        static std::shared_ptr<Layer> createLayer(Engine* engine, const std::string& layerName = "Gizmo",
                                                  int layerIndex = INT_MAX);

        Gizmo(CameraComponent* camera, std::shared_ptr<Layer> layer, const std::string& name = "gizmo");
        ~Gizmo() override;

        Gizmo(const Gizmo&) = delete;
        Gizmo& operator=(const Gizmo&) = delete;

        bool enabled() const;
        void setEnabled(bool state);

        /// Left, middle and right: which buttons interact (upstream `mouseButtons`).
        std::array<bool, 3>& mouseButtons() { return _mouseButtons; }

        const std::shared_ptr<Layer>& layer() const { return _layer; }
        void setLayer(const std::shared_ptr<Layer>& layer);

        CameraComponent* camera() const { return _camera; }
        void setCamera(CameraComponent* camera);

        GizmoSpace coordSpace() const { return _coordSpace; }
        virtual void setCoordSpace(GizmoSpace value);

        float size() const { return _size; }
        void setSize(float value);

        /// The world scale the size resolves to this frame (upstream `_scale`).
        float worldScale() const { return _scale; }

        const std::vector<GraphNode*>& nodes() const { return _nodes; }
        Entity* root() const { return _root; }
        Engine* engine() const { return _engine; }

        /// Upstream `preventDefault`. There is no DOM event to cancel; kept for the API.
        bool preventDefault = true;

        /// Upstream `attach(nodes)`. An empty list is ignored.
        void attach(const std::vector<GraphNode*>& nodes);
        void attach(GraphNode* node);
        void detach();

        virtual void prerender() {}
        virtual void update();

        /// Upstream `destroy()`: detaches and releases every entity, layer slot and
        /// subscription. Called by the destructor, and by the engine's `destroy` event.
        virtual void destroy();

        /// The pointer entry points the mouse device drives. `button` is upstream's
        /// MouseEvent.button (0 left, 1 middle, 2 right). Public so an application
        /// with its own input, or a test, can drive the gizmo directly.
        void pointerDown(float x, float y, int button);
        void pointerMove(float x, float y);
        void pointerUp(float x, float y, int button);

        /// Upstream `_getSelection`: the first mesh instance of the nearest (or highest
        /// priority) shape under canvas point (x, y), or null.
        MeshInstance* getSelection(float x, float y) const;

    protected:
        Vector3 facingDir() const;
        Vector3 cameraDir() const;

        void updatePosition();
        void updateRotation();
        void updateScale();

        /// The axes of the root entity in world space (upstream `root.right / up / forward`).
        Vector3 rootRight() const;
        Vector3 rootUp() const;
        Vector3 rootForward() const;
        Vector3 rootPosition() const;
        Quaternion rootRotation() const;

        Vector3 cameraPosition() const;
        Vector3 cameraForward() const;
        Vector3 cameraRightAxis() const;
        Vector3 cameraUpAxis() const;
        Quaternion cameraRotation() const;

        float _size = 1.0f;
        float _scale = 1.0f;
        GizmoSpace _coordSpace = GizmoSpace::World;
        Engine* _engine = nullptr;
        std::array<bool, 3> _mouseButtons = {true, true, true};
        CameraComponent* _camera = nullptr;
        std::shared_ptr<Layer> _layer;
        bool _renderUpdate = false;
        std::vector<GraphNode*> _nodes;
        Entity* _root = nullptr;
        std::vector<Shape*> _intersectShapes;

    private:
        void addLayerToCamera();
        void removeLayerFromCamera();
        void watchNodes();
        void unwatchNodes();
        void onNodeDestroyed(GraphNode* node);

        std::vector<EventHandlePtr> _handles;
        std::vector<EventHandlePtr> _nodeHandles;
        EventHandlePtr _cameraDestroyed;
        bool _destroyed = false;
        bool _captured = false;
    };
}
