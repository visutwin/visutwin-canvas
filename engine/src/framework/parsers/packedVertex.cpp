// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
// The parsers' shared tangent helpers and packed mesh upload (see packedVertex.h).
//
#include "packedVertex.h"

#include <cmath>
#include <cstring>

#include "core/math/vector3.h"
#include "core/shape/boundingBox.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/mesh.h"

namespace visutwin::canvas
{
    void generateTangents(std::vector<PackedVertex>& vertices, const std::vector<uint32_t>& indices)
    {
        const size_t vertexCount = vertices.size();
        if (vertexCount == 0) {
            return;
        }

        std::vector<Vector3> tan1(vertexCount, Vector3(0.0f, 0.0f, 0.0f));
        std::vector<Vector3> tan2(vertexCount, Vector3(0.0f, 0.0f, 0.0f));

        auto accumulateTriangle = [&](const uint32_t i0, const uint32_t i1, const uint32_t i2) {
            if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) {
                return;
            }

            const auto& v0 = vertices[i0];
            const auto& v1 = vertices[i1];
            const auto& v2 = vertices[i2];

            const Vector3 p0(v0.px, v0.py, v0.pz);
            const Vector3 p1(v1.px, v1.py, v1.pz);
            const Vector3 p2(v2.px, v2.py, v2.pz);

            const float du1 = v1.u - v0.u;
            const float dv1 = v1.v - v0.v;
            const float du2 = v2.u - v0.u;
            const float dv2 = v2.v - v0.v;

            const float det = du1 * dv2 - dv1 * du2;
            if (std::abs(det) <= 1e-8f) {
                return;
            }

            const float invDet = 1.0f / det;
            const Vector3 e1 = p1 - p0;
            const Vector3 e2 = p2 - p0;

            const Vector3 sdir = (e1 * dv2 - e2 * dv1) * invDet;
            const Vector3 tdir = (e2 * du1 - e1 * du2) * invDet;

            tan1[i0] += sdir;
            tan1[i1] += sdir;
            tan1[i2] += sdir;

            tan2[i0] += tdir;
            tan2[i1] += tdir;
            tan2[i2] += tdir;
        };

        if (!indices.empty()) {
            for (size_t i = 0; i + 2 < indices.size(); i += 3) {
                accumulateTriangle(indices[i], indices[i + 1], indices[i + 2]);
            }
        } else {
            for (uint32_t i = 0; i + 2 < vertexCount; i += 3) {
                accumulateTriangle(i, i + 1, i + 2);
            }
        }

        for (size_t i = 0; i < vertexCount; ++i) {
            const Vector3 n(vertices[i].nx, vertices[i].ny, vertices[i].nz);
            Vector3 t = tan1[i] - n * n.dot(tan1[i]);
            if (t.lengthSquared() <= 1e-8f) {
                // Fallback axis in case UVs are degenerate on this vertex.
                t = std::abs(n.getY()) < 0.999f ? n.cross(Vector3(0.0f, 1.0f, 0.0f)) : n.cross(Vector3(1.0f, 0.0f, 0.0f));
            }
            t = t.normalized();

            const float handedness = (n.cross(t).dot(tan2[i]) < 0.0f) ? -1.0f : 1.0f;

            vertices[i].tx = t.getX();
            vertices[i].ty = t.getY();
            vertices[i].tz = t.getZ();
            vertices[i].tw = handedness;
        }
    }

    void tangentFromNormal(const float nx, const float ny, const float nz, float& tx, float& ty, float& tz, float& tw)
    {
        const Vector3 n(nx, ny, nz);
        const Vector3 up = std::abs(ny) < 0.999f ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
        const Vector3 t = n.cross(up).normalized();
        tx = t.getX();
        ty = t.getY();
        tz = t.getZ();
        tw = 1.0f;
    }

    std::shared_ptr<Mesh> createPackedMesh(GraphicsDevice& device, const std::shared_ptr<VertexFormat>& format,
        const std::vector<PackedVertex>& vertices, const std::vector<uint32_t>& indices,
        const Vector3& minPos, const Vector3& maxPos)
    {
        const int vertexCount = static_cast<int>(vertices.size());
        std::vector<uint8_t> vertexBytes(vertices.size() * sizeof(PackedVertex));
        std::memcpy(vertexBytes.data(), vertices.data(), vertexBytes.size());

        VertexBufferOptions vbOpts;
        vbOpts.usage = BUFFER_STATIC;
        vbOpts.data = std::move(vertexBytes);
        auto vb = device.createVertexBuffer(format, vertexCount, vbOpts);

        const int indexCount = static_cast<int>(indices.size());
        const IndexFormat indexFormat = vertexCount <= 65535 ? INDEXFORMAT_UINT16 : INDEXFORMAT_UINT32;
        std::vector<uint8_t> indexBytes;
        if (indexFormat == INDEXFORMAT_UINT16) {
            indexBytes.resize(indices.size() * sizeof(uint16_t));
            auto* dst = reinterpret_cast<uint16_t*>(indexBytes.data());
            for (size_t i = 0; i < indices.size(); ++i) {
                dst[i] = static_cast<uint16_t>(indices[i]);
            }
        } else {
            indexBytes.resize(indices.size() * sizeof(uint32_t));
            std::memcpy(indexBytes.data(), indices.data(), indexBytes.size());
        }
        auto ib = device.createIndexBuffer(indexFormat, indexCount, indexBytes);

        auto mesh = std::make_shared<Mesh>();
        mesh->setVertexBuffer(vb);
        mesh->setIndexBuffer(ib, 0);

        Primitive primitive;
        primitive.type = PRIMITIVE_TRIANGLES;
        primitive.base = 0;
        primitive.baseVertex = 0;
        primitive.count = indexCount;
        primitive.indexed = true;
        mesh->setPrimitive(primitive, 0);

        BoundingBox bounds;
        bounds.setCenter((minPos + maxPos) * 0.5f);
        bounds.setHalfExtents((maxPos - minPos) * 0.5f);
        mesh->setAabb(bounds);
        return mesh;
    }
}
