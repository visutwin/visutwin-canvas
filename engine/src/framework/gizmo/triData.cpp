// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
#include "triData.h"

#include "core/math/vector4.h"
#include "framework/components/render/primitiveGeometry.h"

namespace visutwin::canvas
{
    TriData::TriData(const PrimitiveGeometry& geometry, const int priority)
        : _priority(priority)
    {
        fromGeometry(geometry);
    }

    void TriData::setTransform(const Vector3& position, const Quaternion& rotation, const Vector3& scale)
    {
        _transform = Matrix4::trs(position, rotation, scale);
    }

    void TriData::fromGeometry(const PrimitiveGeometry& geometry)
    {
        const auto& positions = geometry.positions;
        const auto& indices = geometry.indices;
        _tris.clear();
        _tris.reserve(indices.size() / 3);
        for (size_t k = 0; k + 2 < indices.size(); k += 3) {
            const auto vertex = [&](const uint32_t i) {
                return Vector3(positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]);
            };
            _tris.emplace_back(vertex(indices[k]), vertex(indices[k + 1]), vertex(indices[k + 2]));
        }
    }

    bool TriData::intersect(const Matrix4& parentWorld, const Vector3& origin, const Vector3& direction,
        float& outDistance) const
    {
        // Combine the node's world transform with the transform of the triangles relative
        // to it, and carry the ray into that space.
        const Matrix4 triWorld = parentWorld * _transform;
        const Matrix4 invTriWorld = triWorld.inverse();

        const Vector3 localOrigin = invTriWorld.transformPoint(origin);
        const Vector3 localDirection = Vector3(invTriWorld * Vector4(direction, 0.0f)).normalized();
        const Ray ray(localOrigin, localDirection);

        bool hit = false;
        Vector3 point;
        for (const Tri& tri : _tris) {
            if (tri.intersectsRay(ray, &point)) {
                const float distance = (triWorld.transformPoint(point) - origin).length();
                if (!hit || distance < outDistance) {
                    outDistance = distance;
                }
                hit = true;
            }
        }
        return hit;
    }
}
