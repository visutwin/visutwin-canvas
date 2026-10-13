// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
#include "collisionComponent.h"

#include "framework/components/componentRegistry.h"

#include <cstring>

#include "core/math/matrix4.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/entity.h"
#include "framework/physics/physicsWorld.h"
#include "platform/graphics/indexBuffer.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"

namespace visutwin::canvas
{
    CollisionComponent::CollisionComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        listInstance(this);
    }

    CollisionComponent::~CollisionComponent()
    {
        unlistInstance();
    }

    void CollisionComponent::onDisable()
    {
        if (Entity* owner = entity()) {
            if (auto* body = owner->findComponent<RigidBodyComponent>()) {
                body->removeFromSimulation();
            }
        }
        if (RigidBodyComponent* compound = compoundBody()) {
            compound->rebuildBody();
        }
    }

    void CollisionComponent::onEnable()
    {
        if (RigidBodyComponent* compound = compoundBody()) {
            compound->rebuildBody();
        }
    }

    Vector3 CollisionComponent::shapePosition() const
    {
        Entity* owner = entity();
        return owner ? owner->position() + owner->rotation() * _linearOffset : _linearOffset;
    }

    Quaternion CollisionComponent::shapeRotation() const
    {
        Entity* owner = entity();
        return owner ? owner->rotation() * _angularOffset : _angularOffset;
    }

    RigidBodyComponent* CollisionComponent::compoundBody() const
    {
        Entity* owner = entity();
        if (!owner || isCompound() || owner->findComponent<RigidBodyComponent>()) {
            return nullptr;
        }
        for (GraphNode* node = owner->parent(); node; node = node->parent()) {
            auto* ancestor = dynamic_cast<Entity*>(node);
            if (!ancestor) {
                break;
            }
            auto* body = ancestor->findComponent<RigidBodyComponent>();
            const auto* collision = ancestor->findComponent<CollisionComponent>();
            if (collision && collision->isCompound()) {
                return body;
            }
            if (body) {
                // A body of another kind in between owns everything beneath it.
                return nullptr;
            }
        }
        return nullptr;
    }

    namespace
    {
        PhysicsShapeType physicsShapeFor(const std::string& type)
        {
            if (type == "sphere") { return PhysicsShapeType::Sphere; }
            if (type == "capsule") { return PhysicsShapeType::Capsule; }
            if (type == "cylinder") { return PhysicsShapeType::Cylinder; }
            if (type == "plane") { return PhysicsShapeType::Plane; }
            if (type == "cone") { return PhysicsShapeType::Cone; }
            if (type == "mesh") { return PhysicsShapeType::Mesh; }
            if (type == "compound") { return PhysicsShapeType::Compound; }
            return PhysicsShapeType::Box;
        }

        // The compound's child shapes beneath `node`, each placed in the compound's space by
        // composing the LOCAL transforms down from it (`position`, `rotation` and `scale` are
        // `node`'s there). Taken from world transforms instead, the offsets would carry their
        // rounding, and a Debug and a Release build would drop a pile of compounds differently.
        // The compound's own scale is not in them, as it is in no single shape's size.
        void collectCompoundChildren(GraphNode* node, const Vector3& position, const Quaternion& rotation,
            const Vector3& scale, const bool dynamic, std::vector<PhysicsShapeDesc>& children)
        {
            for (const auto& owned : node->children()) {
                auto* child = dynamic_cast<Entity*>(owned.get());
                if (!child || !child->enabled()) {
                    continue;
                }
                const auto* collision = child->findComponent<CollisionComponent>();
                if (child->findComponent<RigidBodyComponent>() || (collision && collision->isCompound())) {
                    continue;
                }
                const Vector3 childPosition = position + rotation * (scale * child->localPosition());
                const Quaternion childRotation = rotation * child->localRotation();
                if (collision && collision->enabled()) {
                    PhysicsShapeDesc shape;
                    if (collision->describeShape(shape, nullptr, dynamic)) {
                        shape.offsetPosition = childPosition + childRotation * collision->linearOffset();
                        shape.offsetRotation = (childRotation * collision->angularOffset()).normalized();
                        children.push_back(std::move(shape));
                    }
                }
                collectCompoundChildren(child, childPosition, childRotation, scale * child->localScale(), dynamic,
                    children);
            }
        }
    }

    bool CollisionComponent::describeShape(PhysicsShapeDesc& out, std::vector<PhysicsShapeDesc>* children,
        const bool dynamic) const
    {
        out.shape = physicsShapeFor(_type);
        out.halfExtents = _halfExtents;
        out.radius = _radius;
        out.height = _height;
        out.offsetPosition = _linearOffset;
        out.offsetRotation = _angularOffset;
        if (out.shape == PhysicsShapeType::Mesh) {
            collectMeshGeometry(out.points, out.indices);
            // Jolt simulates a triangle mesh only on a static or kinematic body.
            if (_convexHull || dynamic) {
                out.shape = PhysicsShapeType::ConvexHull;
            }
            return out.points.size() >= 4 && (out.shape != PhysicsShapeType::Mesh || !out.indices.empty());
        }
        if (out.shape == PhysicsShapeType::Compound) {
            // The children carry their own places; the compound itself sits at the body's origin.
            out.offsetPosition = Vector3(0.0f, 0.0f, 0.0f);
            out.offsetRotation = Quaternion();
            if (!children || !entity()) {
                return false;
            }
            children->clear();
            collectCompoundChildren(entity(), Vector3(0.0f, 0.0f, 0.0f), Quaternion(), Vector3(1.0f, 1.0f, 1.0f),
                dynamic, *children);
            return !children->empty();
        }
        return true;
    }

    BoundingSphere CollisionComponent::worldBounds() const
    {
        if (!entity()) {
            return BoundingSphere(Vector3(0.0f, 0.0f, 0.0f), _radius);
        }

        // A conservative sphere for the CPU raycast fallback, used only for shape types it
        // cannot intersect analytically (anything but box, sphere and capsule); the physics
        // world builds its own shapes. Taken from the render bounds where there are some.
        if (const auto* render = entity()->findComponent<RenderComponent>(); render && !render->meshInstances().empty()) {
            bool hasBounds = false;
            BoundingBox merged;
            for (auto* meshInstance : render->meshInstances()) {
                if (!meshInstance) {
                    continue;
                }
                const BoundingBox worldAabb = meshInstance->aabb();
                if (!hasBounds) {
                    merged = worldAabb;
                    hasBounds = true;
                } else {
                    merged.add(worldAabb);
                }
            }

            if (hasBounds) {
                return BoundingSphere(merged.center(), std::max(merged.halfExtents().length(), 0.001f));
            }
        }

        return BoundingSphere(entity()->position(), std::max(_halfExtents.length(), _radius));
    }

    void CollisionComponent::cloneFrom(const Component* source)
    {
        const auto* src = dynamic_cast<const CollisionComponent*>(source);
        if (!src) {
            return;
        }
        _type = src->_type;
        _halfExtents = src->_halfExtents;
        _radius = src->_radius;
        _height = src->_height;
        _render = src->_render;
        _convexHull = src->_convexHull;
        _linearOffset = src->_linearOffset;
        _angularOffset = src->_angularOffset;
    }

    namespace
    {
        // Appends one mesh's triangles, each position through `transform` and then
        // `scale`. Reads float positions (the parsers' packed layout puts them first; a
        // format with an element list says where). Non-triangle primitives contribute
        // nothing.
        void appendMesh(const Mesh& mesh, const Matrix4& transform, const Vector3& scale,
            std::vector<Vector3>& points, std::vector<uint32_t>& indices)
        {
            const auto vb = mesh.getVertexBuffer();
            if (!vb || vb->storage().empty() || !vb->format()) {
                return;
            }
            const Primitive primitive = mesh.getPrimitive(0);
            if (primitive.type != PrimitiveType::PRIMITIVE_TRIANGLES) {
                return;
            }
            uint32_t positionOffset = 0;
            for (const VertexElement& element : vb->format()->elements()) {
                if (element.semantic == VertexSemantic::SEMANTIC_POSITION) {
                    if (element.dataType != VertexDataType::TYPE_FLOAT32 || element.componentCount < 3) {
                        return;
                    }
                    positionOffset = element.offset;
                }
            }

            const size_t stride = static_cast<size_t>(vb->format()->size());
            const uint8_t* vdata = vb->storage().data();
            const size_t vertexCount = std::min(static_cast<size_t>(vb->numVertices()),
                vb->storage().size() / std::max<size_t>(stride, 1));
            const auto base = static_cast<uint32_t>(points.size());
            for (size_t i = 0; i < vertexCount; ++i) {
                float p[3];
                std::memcpy(p, vdata + i * stride + positionOffset, sizeof(p));
                points.push_back(transform.transformPoint(Vector3(p[0], p[1], p[2])) * scale);
            }

            const auto ib = mesh.getIndexBuffer(0);
            if (primitive.indexed && ib && !ib->storage().empty()) {
                const uint8_t* idx = ib->storage().data();
                const size_t elem = ib->format() == INDEXFORMAT_UINT32 ? 4 : (ib->format() == INDEXFORMAT_UINT16 ? 2 : 1);
                const size_t available = ib->storage().size() / elem;
                const size_t first = static_cast<size_t>(std::max(primitive.base, 0));
                const size_t count = primitive.count > 0 ? static_cast<size_t>(primitive.count) : available;
                const size_t end = std::min(available, first + count);
                for (size_t k = first; k + 2 < end; k += 3) {
                    for (size_t c = 0; c < 3; ++c) {
                        uint32_t v = 0;
                        if (elem == 4) { std::memcpy(&v, idx + (k + c) * 4, 4); }
                        else if (elem == 2) { uint16_t s = 0; std::memcpy(&s, idx + (k + c) * 2, 2); v = s; }
                        else { v = idx[k + c]; }
                        if (v >= vertexCount) {
                            v = 0;
                        }
                        indices.push_back(base + v);
                    }
                }
            } else {
                const size_t first = static_cast<size_t>(std::max(primitive.base, 0));
                const size_t count = primitive.count > 0 ? static_cast<size_t>(primitive.count) : vertexCount;
                const size_t end = std::min(vertexCount, first + count);
                for (size_t k = first; k + 2 < end; k += 3) {
                    indices.push_back(base + static_cast<uint32_t>(k));
                    indices.push_back(base + static_cast<uint32_t>(k + 1));
                    indices.push_back(base + static_cast<uint32_t>(k + 2));
                }
            }
        }
    }

    void CollisionComponent::collectMeshGeometry(std::vector<Vector3>& points, std::vector<uint32_t>& indices) const
    {
        points.clear();
        indices.clear();
        Entity* owner = entity();
        if (!owner) {
            return;
        }
        const Vector3 scale = owner->worldTransform().getScale();
        if (!_render.empty()) {
            for (const auto& mesh : _render) {
                if (mesh) {
                    appendMesh(*mesh, Matrix4::identity(), scale, points, indices);
                }
            }
            return;
        }
        const auto* render = owner->findComponent<RenderComponent>();
        if (!render) {
            return;
        }
        // Each mesh instance's node relative to the entity, without the entity's own
        // scale (which `scale` applies).
        const Matrix4 ownerInverse = Matrix4::trs(owner->position(), owner->rotation(), Vector3(1.0f)).inverse();
        for (const auto* meshInstance : render->meshInstances()) {
            if (!meshInstance || !meshInstance->mesh()) {
                continue;
            }
            const Matrix4 nodeWorld = meshInstance->node() ? meshInstance->node()->worldTransform() : owner->worldTransform();
            Matrix4 relative = ownerInverse * nodeWorld;
            // relative carries the owner's scale; take it out so it is applied once.
            const Vector3 inverseScale(scale.getX() != 0.0f ? 1.0f / scale.getX() : 0.0f,
                scale.getY() != 0.0f ? 1.0f / scale.getY() : 0.0f,
                scale.getZ() != 0.0f ? 1.0f / scale.getZ() : 0.0f);
            relative = Matrix4::trs(Vector3(0.0f), Quaternion(), inverseScale) * relative;
            appendMesh(*meshInstance->mesh(), relative, scale, points, indices);
        }
    }
}
