// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Upstream extras/gizmo/shape/shape.js: one interactive part of a gizmo — an entity
// with render components drawing unit primitives through a GizmoMaterial, plus the
// TriData it is picked against.
//
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/math/color.h"
#include "core/math/vector3.h"
#include "framework/gizmo/gizmoConstants.h"
#include "framework/gizmo/triData.h"
#include "platform/graphics/constants.h"

namespace visutwin::canvas
{
    class Engine;
    class Entity;
    class GizmoMaterial;
    class GraphicsDevice;
    class Mesh;
    class MeshInstance;
    struct PrimitiveGeometry;

    /// Upstream ShapeArgs. The colours are POINTERS into a gizmo theme (upstream passes the
    /// theme's Color objects by reference), so a theme change reaches the shape.
    struct ShapeArgs
    {
        GizmoAxis axis = GizmoAxis::X;
        Vector3 position = Vector3(0.0f);
        Vector3 rotation = Vector3(0.0f);   ///< Euler angles in degrees
        Vector3 scale = Vector3(1.0f);
        bool disabled = false;
        bool visible = true;
        std::vector<int> layers;
        const Color* defaultColor = nullptr;
        const Color* hoverColor = nullptr;
        const Color* disabledColor = nullptr;
        std::optional<CullMode> cull;
        /// -1 = interpolated depth; a positive value writes that depth (the planes use 1).
        std::optional<float> depth;
    };

    class Shape
    {
    public:
        Shape(Engine* engine, const std::string& name, const ShapeArgs& args);
        virtual ~Shape();

        Shape(const Shape&) = delete;
        Shape& operator=(const Shape&) = delete;

        GizmoAxis axis() const { return _axis; }

        /// Upstream identifies a plane by its entity name ('plane:x'); here the shape says so.
        virtual bool isPlane() const { return false; }

        /// The shape's root entity. The shape OWNS it until `takeEntity()` hands it to a
        /// parent (the gizmo root); after that the hierarchy owns it.
        Entity* entity() const { return _entity; }
        std::unique_ptr<Entity> takeEntity();

        const std::vector<const TriData*>& triData() const { return _triData; }
        const std::vector<MeshInstance*>& meshInstances() const { return _meshInstances; }
        bool ownsMeshInstance(const MeshInstance* meshInstance) const;

        bool disabled() const { return _disabled; }
        void setDisabled(bool value);

        bool visible() const { return _visible; }
        void setVisible(bool value);

        /// The colour for the hover state (or the disabled colour when disabled).
        void hover(bool state);

        const std::shared_ptr<GizmoMaterial>& material() const { return _material; }

    protected:
        /// Adds a render component to `entity` drawing each mesh with the shape's material.
        void createRenderComponent(Entity* entity, const std::vector<std::shared_ptr<Mesh>>& meshes);

        /// Replaces the meshes of the render component on `entity`, keeping each instance's
        /// visibility. DEVIATION: upstream swaps `meshInstance.mesh` in place; a MeshInstance
        /// here has no mesh setter, so the instances are rebuilt.
        void replaceMeshes(Entity* entity, const std::vector<std::shared_ptr<Mesh>>& meshes);

        std::shared_ptr<Mesh> createMesh(const PrimitiveGeometry& geometry) const;

        TriData* addTriData(const PrimitiveGeometry& geometry, int priority = 0);

        /// Upstream `_update`: places the entity from the stored transform.
        virtual void update();

        /// A new child entity of `parent` named `name`.
        Entity* createChild(Entity* parent, const std::string& name) const;

        Engine* _engine = nullptr;
        std::shared_ptr<GraphicsDevice> _device;
        GizmoAxis _axis = GizmoAxis::X;

        Vector3 _position = Vector3(0.0f);
        Vector3 _rotation = Vector3(0.0f);
        Vector3 _scale = Vector3(1.0f);
        std::vector<int> _layers;

        std::shared_ptr<GizmoMaterial> _material;
        bool _disabled = false;
        bool _visible = true;
        const Color* _defaultColor = &Color::WHITE;
        const Color* _hoverColor = &Color::BLACK;
        const Color* _disabledColor = &GIZMO_COLOR_GRAY;
        CullMode _cull = CullMode::CULLFACE_BACK;
        float _depth = -1.0f;

        Entity* _entity = nullptr;
        std::unique_ptr<Entity> _ownedEntity;

        std::vector<const TriData*> _triData;
        std::vector<std::unique_ptr<TriData>> _triDataStore;
        std::vector<MeshInstance*> _meshInstances;
    };
}
