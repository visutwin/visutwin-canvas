// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
#include "shape.h"

#include <algorithm>

#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/componentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/gizmo/gizmoMaterial.h"
#include "scene/meshInstance.h"

namespace visutwin::canvas
{
    Shape::Shape(Engine* engine, const std::string& name, const ShapeArgs& args)
        : _engine(engine), _device(engine ? engine->graphicsDevice() : nullptr), _axis(args.axis),
          _position(args.position), _rotation(args.rotation), _scale(args.scale), _layers(args.layers),
          _disabled(args.disabled), _visible(args.visible)
    {
        if (args.defaultColor) {
            _defaultColor = args.defaultColor;
        }
        if (args.hoverColor) {
            _hoverColor = args.hoverColor;
        }
        if (args.disabledColor) {
            _disabledColor = args.disabledColor;
        }
        _cull = args.cull.value_or(_cull);
        _depth = args.depth.value_or(_depth);

        _material = std::make_shared<GizmoMaterial>(_device);

        // entity
        _ownedEntity = std::make_unique<Entity>();
        _entity = _ownedEntity.get();
        _entity->setEngine(engine);
        _entity->setName(name + ":" + gizmoAxisName(_axis));
        _entity->setLocalPosition(_position);
        _entity->setLocalEulerAngles(_rotation.getX(), _rotation.getY(), _rotation.getZ());
        _entity->setLocalScale(_scale);
    }

    Shape::~Shape() = default;

    std::unique_ptr<Entity> Shape::takeEntity()
    {
        return std::move(_ownedEntity);
    }

    bool Shape::ownsMeshInstance(const MeshInstance* meshInstance) const
    {
        return meshInstance &&
            std::find(_meshInstances.begin(), _meshInstances.end(), meshInstance) != _meshInstances.end();
    }

    void Shape::setDisabled(const bool value)
    {
        _disabled = value;
        hover(false);
    }

    void Shape::setVisible(const bool value)
    {
        if (value == _visible) {
            return;
        }
        for (MeshInstance* meshInstance : _meshInstances) {
            meshInstance->setVisible(value);
        }
        _visible = value;
    }

    void Shape::hover(const bool state)
    {
        const Color* color = _disabled ? _disabledColor : state ? _hoverColor : _defaultColor;
        _material->setColor(*color);
    }

    std::shared_ptr<Mesh> Shape::createMesh(const PrimitiveGeometry& geometry) const
    {
        return createMeshFromGeometry(_device, geometry);
    }

    TriData* Shape::addTriData(const PrimitiveGeometry& geometry, const int priority)
    {
        auto& stored = _triDataStore.emplace_back(std::make_unique<TriData>(geometry, priority));
        _triData.push_back(stored.get());
        return stored.get();
    }

    Entity* Shape::createChild(Entity* parent, const std::string& name) const
    {
        auto child = std::make_unique<Entity>();
        Entity* raw = child.get();
        raw->setEngine(_engine);
        raw->setName(name);
        parent->addChild(std::move(child));
        return raw;
    }

    void Shape::createRenderComponent(Entity* entity, const std::vector<std::shared_ptr<Mesh>>& meshes)
    {
        const Color& color = _disabled ? *_disabledColor : *_defaultColor;
        // DEPTH_WRITE only for a positive depth, so -1 (and 0) keep the
        // interpolated one.
        _material->setDepth(_depth > 0.0f ? _depth : -1.0f);
        _material->setColor(color);
        _material->setCullMode(_cull);

        auto* render = entity ? static_cast<RenderComponent*>(entity->addComponent<RenderComponent>()) : nullptr;
        if (!render) {
            return;
        }
        render->setLayers(_layers);
        render->setCastShadows(false);
        render->setReceiveShadows(false);
        for (const auto& mesh : meshes) {
            if (!mesh) {
                continue;
            }
            auto instance = std::make_unique<MeshInstance>(mesh, std::static_pointer_cast<Material>(_material), entity);
            instance->setCull(false);
            instance->setCastShadow(false);
            instance->setReceiveShadow(false);
            instance->setVisible(_visible);
            _meshInstances.push_back(render->addMeshInstance(std::move(instance)));
        }
    }

    void Shape::replaceMeshes(Entity* entity, const std::vector<std::shared_ptr<Mesh>>& meshes)
    {
        auto* render = entity ? entity->findComponent<RenderComponent>() : nullptr;
        if (!render) {
            return;
        }
        std::vector<bool> visibility;
        for (const MeshInstance* instance : render->meshInstances()) {
            visibility.push_back(instance->visible());
        }
        std::erase_if(_meshInstances, [entity](const MeshInstance* instance) {
            return instance->node() == entity;
        });
        render->clearMeshInstances();
        for (size_t i = 0; i < meshes.size(); ++i) {
            if (!meshes[i]) {
                continue;
            }
            auto instance = std::make_unique<MeshInstance>(meshes[i], std::static_pointer_cast<Material>(_material), entity);
            instance->setCull(false);
            instance->setCastShadow(false);
            instance->setReceiveShadow(false);
            instance->setVisible(i < visibility.size() ? visibility[i] : _visible);
            _meshInstances.push_back(render->addMeshInstance(std::move(instance)));
        }
    }

    void Shape::update()
    {
        _entity->setLocalPosition(_position);
        _entity->setLocalEulerAngles(_rotation.getX(), _rotation.getY(), _rotation.getZ());
        _entity->setLocalScale(_scale);
    }
}
