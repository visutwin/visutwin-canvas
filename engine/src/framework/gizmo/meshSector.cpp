// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 20.10.2026
//
#include "meshSector.h"

#include <algorithm>

#include "core/math/quaternion.h"
#include "framework/components/componentSystem.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/render/renderComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/gizmo/gizmoMaterial.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/vertexBuffer.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"

namespace visutwin::canvas
{
    namespace
    {
        // The standard packed vertex: position, normal, uv0, tangent, uv1.
        constexpr size_t kFloatsPerVertex = 14;
    }

    MeshSector::MeshSector(Engine* engine, const int layerId, const int segments)
        : _segments(std::max(segments, 1))
    {
        if (!engine || !engine->root() || !engine->graphicsDevice()) {
            return;
        }
        const auto& device = engine->graphicsDevice();
        _material = std::make_shared<GizmoMaterial>(device);
        _material->setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));
        auto depth = std::make_shared<DepthState>();
        depth->setDepthWrite(false);
        _material->setDepthState(depth);
        _material->setCullMode(CullMode::CULLFACE_NONE);

        auto entity = std::make_unique<Entity>();
        _entity = entity.get();
        _entity->setEngine(engine);
        _entity->setName("mesh-sector");
        engine->root()->addChild(std::move(entity));

        auto* render = static_cast<RenderComponent*>(_entity->addComponent<RenderComponent>());
        if (!render) {
            return;
        }
        render->setLayers({layerId});
        render->setCastShadows(false);

        // A fan around the centre vertex; the positions are written on each draw.
        PrimitiveGeometry geometry;
        const size_t vertexCount = static_cast<size_t>(_segments) + 2u;
        geometry.positions.assign(vertexCount * 3u, 0.0f);
        geometry.normals.assign(vertexCount * 3u, 0.0f);
        geometry.uvs.assign(vertexCount * 2u, 0.0f);
        for (int i = 0; i < _segments; ++i) {
            geometry.indices.insert(geometry.indices.end(), {0u, static_cast<uint32_t>(i + 1),
                static_cast<uint32_t>(i + 2)});
        }
        _vertices.assign(vertexCount * kFloatsPerVertex, 0.0f);

        const int copies = std::max(device->maxFramesInFlight(), 1);
        for (int i = 0; i < copies; ++i) {
            auto mesh = createMeshFromGeometry(device, geometry);
            auto instance = std::make_unique<MeshInstance>(mesh, std::static_pointer_cast<Material>(_material),
                _entity);
            instance->setCull(false);
            instance->setCastShadow(false);
            instance->setVisible(i == 0);
            _instances.push_back(render->addMeshInstance(std::move(instance)));
            _meshes.push_back(std::move(mesh));
        }
    }

    MeshSector::~MeshSector()
    {
        if (_entity && _entity->parent()) {
            auto owned = _entity->remove();
        }
        _entity = nullptr;
    }

    std::vector<Vector3> MeshSector::fanPositions(const Vector3& center, const Vector3& start, const Vector3& axis,
        const float from, const float sweep, const float scale, const int segments)
    {
        std::vector<Vector3> positions;
        positions.reserve(static_cast<size_t>(segments) + 2u);
        positions.push_back(center);
        for (int i = 0; i <= segments; ++i) {
            const float angle = from + sweep * static_cast<float>(i) / static_cast<float>(segments);
            positions.push_back(center + (Quaternion::fromAxisAngle(axis, angle) * start) * scale);
        }
        return positions;
    }

    void MeshSector::draw(const Vector3& center, const Vector3& start, const Vector3& axis, const float from,
        const float sweep, const float scale, const Color& color)
    {
        if (!_entity || _meshes.empty() || !_material) {
            return;
        }
        _material->setColor(color);

        // A new frame writes the next mesh; a second draw in the same frame rewrites its own.
        const auto& device = _entity->engine() ? _entity->engine()->graphicsDevice() : nullptr;
        const int version = device ? device->renderVersion() : 0;
        if (version != _writtenVersion) {
            _writtenVersion = version;
            _slot = (_slot + 1) % _meshes.size();
        }

        const auto positions = fanPositions(center, start, axis, from, sweep, scale, _segments);
        for (size_t i = 0; i < positions.size(); ++i) {
            positions[i].store(&_vertices[i * kFloatsPerVertex]);
        }
        if (const auto buffer = _meshes[_slot]->getVertexBuffer()) {
            buffer->writeRange(0, _vertices.data(), _vertices.size() * sizeof(float));
        }
        for (size_t i = 0; i < _instances.size(); ++i) {
            if (_instances[i]) {
                _instances[i]->setVisible(i == _slot);
            }
        }
    }
}
