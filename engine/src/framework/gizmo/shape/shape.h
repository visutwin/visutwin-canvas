// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// One interactive part of a gizmo — an entity
// with render components drawing unit primitives through a GizmoMaterial, plus the
// TriData it is picked against. With an occluded colour, each mesh also draws an occluded
// pass first, without depth test, so the parts the scene hides show faint.
//
#pragma once

#include <cstdint>
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
    class RenderComponent;
    struct PrimitiveGeometry;

    /// Shape construction arguments. The colours are POINTERS into a gizmo theme, so a theme
    /// change reaches the shape.
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
        /// The colour where the scene hides the shape, drawn first without depth test; null
        /// for no occluded pass.
        const Color* occludedColor = nullptr;
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

        /// Whether this shape is a plane handle.
        virtual bool isPlane() const { return false; }

        /// The shape's root entity. The shape OWNS it until `takeEntity()` hands it to a
        /// parent (the gizmo root); after that the hierarchy owns it.
        Entity* entity() const { return _entity; }
        std::unique_ptr<Entity> takeEntity();

        const std::vector<const TriData*>& triData() const { return _triData; }
        /// The visible pass's mesh instances, which picking reports.
        const std::vector<MeshInstance*>& meshInstances() const { return _meshInstances; }
        /// The occluded pass's mesh instances, one per visible one (none without an
        /// occluded colour).
        const std::vector<MeshInstance*>& occludedMeshInstances() const { return _occludedMeshInstances; }
        bool ownsMeshInstance(const MeshInstance* meshInstance) const;
        /// Counts the times the shape rebuilt its mesh instances (replaceMeshes), so a holder of
        /// their pointers knows to collect them again.
        uint32_t meshVersion() const { return _meshVersion; }

        bool disabled() const { return _disabled; }
        void setDisabled(bool value);

        bool visible() const { return _visible; }
        virtual void setVisible(bool value);

        /// A factor of the alpha of every colour.
        float fade() const { return _fade; }
        void setFade(float value);

        /// The colour for the hover state (or the disabled colour when disabled), its alpha
        /// scaled by the fade.
        virtual void hover(bool state);
        bool hovered() const { return _hovered; }

        /// Polygon offset of the visible pass, so the shape does not fight a surface it lies
        /// on. Positive values push it away from the camera.
        void setDepthBias(float value);
        float depthBias() const;
        /// Whether the visible pass writes the depth buffer.
        void setDepthWrite(bool value);
        bool depthWrite() const;

        const std::shared_ptr<GizmoMaterial>& material() const { return _material; }
        const std::shared_ptr<GizmoMaterial>& occludedMaterial() const { return _occludedMaterial; }

    protected:
        /// Adds a render component to `entity` drawing each mesh with the shape's material.
        void createRenderComponent(Entity* entity, const std::vector<std::shared_ptr<Mesh>>& meshes);

        /// Replaces the meshes of the render component on `entity`, keeping each instance's
        /// visibility. DEVIATION: upstream swaps `meshInstance.mesh` in place; a MeshInstance
        /// here has no mesh setter, so the instances are rebuilt.
        void replaceMeshes(Entity* entity, const std::vector<std::shared_ptr<Mesh>>& meshes);

        /// Adds each mesh to `render` on `entity`, its occluded pass first, each pair visible as
        /// `visibility` says (the shape's visibility past its end).
        void addMeshInstances(RenderComponent* render, Entity* entity,
            const std::vector<std::shared_ptr<Mesh>>& meshes, const std::vector<bool>& visibility);

        std::shared_ptr<Mesh> createMesh(const PrimitiveGeometry& geometry) const;

        TriData* addTriData(const PrimitiveGeometry& geometry, int priority = 0);

        /// Places the entity from the stored transform.
        virtual void update();

        /// The colour of the current state, its alpha scaled by the fade and `factor`.
        Color stateColor(float factor = 1.0f) const;

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
        const Color* _occludedColor = nullptr;
        std::shared_ptr<GizmoMaterial> _occludedMaterial;
        bool _hovered = false;
        float _fade = 1.0f;
        CullMode _cull = CullMode::CULLFACE_BACK;
        float _depth = -1.0f;

        Entity* _entity = nullptr;
        std::unique_ptr<Entity> _ownedEntity;

        std::vector<const TriData*> _triData;
        std::vector<std::unique_ptr<TriData>> _triDataStore;
        std::vector<MeshInstance*> _meshInstances;
        std::vector<MeshInstance*> _occludedMeshInstances;
        uint32_t _meshVersion = 0;
    };
}
