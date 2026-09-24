// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.02.2026.
//
#include "renderComponent.h"
#include "primitiveGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

#include <spdlog/spdlog.h>

#include "framework/batching/batchManager.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/geometry/geometryUtils.h"
#include "scene/skinInstance.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr float PI_F = 3.14159265358979323846f;

        void pushVertex(PrimitiveGeometry& geometry,
            const float px, const float py, const float pz,
            const float nx, const float ny, const float nz,
            const float u, const float v)
        {
            geometry.positions.insert(geometry.positions.end(), {px, py, pz});
            geometry.normals.insert(geometry.normals.end(), {nx, ny, nz});
            geometry.uvs.insert(geometry.uvs.end(), {u, v});
        }

        // Upstream's primitiveUv1Padding: every lightmap cell keeps an 8/64 border of
        // its own size free, so bilinear taps and the bake's dilation stay inside it.
        constexpr float kUv1Padding = 8.0f / 64.0f;
        constexpr float kUv1PaddingScale = 1.0f - kUv1Padding * 2.0f;

        // One UV1 in upstream's layout. (u, v) are upstream's UNFLIPPED coordinates in
        // [0, 1] for the part; they are padded, scaled into the part's cell and offset
        // to it, and stored as (u, 1 - v) like every other uv here.
        void pushUv1(PrimitiveGeometry& geometry, const float u, const float v,
            const float scaleU, const float scaleV, const float offsetU, const float offsetV)
        {
            const float cellU = (u * kUv1PaddingScale + kUv1Padding) * scaleU + offsetU;
            const float cellV = (v * kUv1PaddingScale + kUv1Padding) * scaleV + offsetV;
            geometry.uvs1.insert(geometry.uvs1.end(), {cellU, 1.0f - cellV});
        }

        // Every primitive's tangent frame is derived from its UVs rather than written
        // by hand: hand-written frames gave the box (1, 0, 0) on every face — parallel
        // to the normal on +/-X — and the sphere and capsule caps a reversed tangent.
        PrimitiveGeometry withTangents(PrimitiveGeometry geometry)
        {
            geometry.tangents = calculateTangents(geometry.positions, geometry.normals, geometry.uvs, geometry.indices);
            return geometry;
        }

        std::shared_ptr<Mesh> createMesh(const std::shared_ptr<GraphicsDevice>& device, const PrimitiveGeometry& geometry)
        {
            if (!device || geometry.positions.empty() || geometry.indices.empty()) {
                return nullptr;
            }

            const int vertexCount = static_cast<int>(geometry.positions.size() / 3);
            // A primitive without an unwrap of its own uses its UV0 as UV1 (upstream's
            // plane and sphere set `uvs1 = uvs`).
            const std::vector<float>& uvs1 =
                geometry.uvs1.size() == geometry.uvs.size() ? geometry.uvs1 : geometry.uvs;
            std::vector<float> interleaved;
            interleaved.reserve(static_cast<size_t>(vertexCount) * 14u);

            Vector3 minBounds(
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max());
            Vector3 maxBounds(
                std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest());

            for (int i = 0; i < vertexCount; ++i) {
                const size_t posOffset = static_cast<size_t>(i) * 3u;
                const size_t uvOffset = static_cast<size_t>(i) * 2u;

                const float px = geometry.positions[posOffset];
                const float py = geometry.positions[posOffset + 1u];
                const float pz = geometry.positions[posOffset + 2u];
                const float nx = geometry.normals[posOffset];
                const float ny = geometry.normals[posOffset + 1u];
                const float nz = geometry.normals[posOffset + 2u];
                const size_t tanOffset = static_cast<size_t>(i) * 4u;
                const bool hasTangents = geometry.tangents.size() >= static_cast<size_t>(vertexCount) * 4u;
                const float tx = hasTangents ? geometry.tangents[tanOffset] : 0.0f;
                const float ty = hasTangents ? geometry.tangents[tanOffset + 1u] : 0.0f;
                const float tz = hasTangents ? geometry.tangents[tanOffset + 2u] : 0.0f;
                const float tw = hasTangents ? geometry.tangents[tanOffset + 3u] : 1.0f;
                const float u = geometry.uvs[uvOffset];
                const float v = geometry.uvs[uvOffset + 1u];
                const float u1 = uvs1[uvOffset];
                const float v1 = uvs1[uvOffset + 1u];

                const Vector3 position(px, py, pz);
                minBounds = Vector3::min(minBounds, position);
                maxBounds = Vector3::max(maxBounds, position);

                interleaved.insert(interleaved.end(), {
                    px, py, pz,
                    nx, ny, nz,
                    u, v,
                    tx, ty, tz, tw,
                    u1, v1
                });
            }

            std::vector<uint8_t> vertexBytes(interleaved.size() * sizeof(float));
            std::memcpy(vertexBytes.data(), interleaved.data(), vertexBytes.size());
            VertexBufferOptions vbOptions;
            vbOptions.data = std::move(vertexBytes);

            auto vertexFormat = std::make_shared<VertexFormat>(
                14 * static_cast<int>(sizeof(float)), VertexFormat::standardElements(), true, false);
            auto vertexBuffer = device->createVertexBuffer(vertexFormat, vertexCount, vbOptions);

            std::vector<uint8_t> indexBytes(geometry.indices.size() * sizeof(uint32_t));
            std::memcpy(indexBytes.data(), geometry.indices.data(), indexBytes.size());
            auto indexBuffer = device->createIndexBuffer(
                INDEXFORMAT_UINT32,
                static_cast<int>(geometry.indices.size()),
                indexBytes);

            auto mesh = std::make_shared<Mesh>();
            mesh->setVertexBuffer(vertexBuffer);
            mesh->setIndexBuffer(indexBuffer, 0);

            Primitive primitive;
            primitive.type = PRIMITIVE_TRIANGLES;
            primitive.base = 0;
            primitive.baseVertex = 0;
            primitive.count = static_cast<int>(geometry.indices.size());
            primitive.indexed = true;
            mesh->setPrimitive(primitive, 0);

            const auto center = (minBounds + maxBounds) * 0.5f;
            const auto halfExtents = (maxBounds - minBounds) * 0.5f;
            BoundingBox bounds;
            bounds.setCenter(center);
            bounds.setHalfExtents(halfExtents);
            mesh->setAabb(bounds);

            return mesh;
        }
    }

    // Declared in primitiveGeometry.h. See there for the texture-origin convention
    // every primitive follows.

        PrimitiveGeometry createBoxGeometry()
        {
            PrimitiveGeometry geometry;

            constexpr float halfExtent = 0.5f;
            constexpr int uSegments = 1;
            constexpr int vSegments = 1;

            const std::array<Vector3, 8> corners = {{
                {-halfExtent, -halfExtent, halfExtent},
                {halfExtent, -halfExtent, halfExtent},
                {halfExtent, halfExtent, halfExtent},
                {-halfExtent, halfExtent, halfExtent},
                {halfExtent, -halfExtent, -halfExtent},
                {-halfExtent, -halfExtent, -halfExtent},
                {-halfExtent, halfExtent, -halfExtent},
                {halfExtent, halfExtent, -halfExtent}
            }};

            const int faceAxes[6][3] = {
                {0, 1, 3},
                {4, 5, 7},
                {3, 2, 6},
                {1, 0, 4},
                {1, 4, 2},
                {5, 0, 6}
            };

            const float faceNormals[6][3] = {
                {0.0f, 0.0f, 1.0f},
                {0.0f, 0.0f, -1.0f},
                {0.0f, 1.0f, 0.0f},
                {0.0f, -1.0f, 0.0f},
                {1.0f, 0.0f, 0.0f},
                {-1.0f, 0.0f, 0.0f}
            };

            geometry.positions.reserve(6u * 4u * 3u);
            geometry.normals.reserve(6u * 4u * 3u);
            geometry.uvs.reserve(6u * 4u * 2u);
            geometry.indices.reserve(6u * 6u);

            uint32_t vertexCounter = 0;
            for (int side = 0; side < 6; ++side) {
                for (int i = 0; i <= uSegments; ++i) {
                    for (int j = 0; j <= vSegments; ++j) {
                        const float u = static_cast<float>(i) / static_cast<float>(uSegments);
                        const float v = static_cast<float>(j) / static_cast<float>(vSegments);

                        const auto& c0 = corners[faceAxes[side][0]];
                        const auto& c1 = corners[faceAxes[side][1]];
                        const auto& c2 = corners[faceAxes[side][2]];

                        const Vector3 temp1 = Vector3::lerp(c0, c1, u);
                        const Vector3 temp2 = Vector3::lerp(c0, c2, v);
                        const Vector3 position = temp1 + (temp2 - c0);

                        pushVertex(
                            geometry,
                            position.getX(), position.getY(), position.getZ(),
                            faceNormals[side][0], faceNormals[side][1], faceNormals[side][2],
                            u, 1.0f - v);
                        // Upstream packs the six faces 3x2, one face per cell (the
                        // top third of the square stays empty rather than stretching).
                        pushUv1(geometry, u, v, 1.0f / 3.0f, 1.0f / 3.0f,
                            static_cast<float>(side % 3) / 3.0f, static_cast<float>(side / 3) / 3.0f);

                        if (i < uSegments && j < vSegments) {
                            geometry.indices.push_back(vertexCounter + static_cast<uint32_t>(vSegments + 1));
                            geometry.indices.push_back(vertexCounter + 1u);
                            geometry.indices.push_back(vertexCounter);
                            geometry.indices.push_back(vertexCounter + static_cast<uint32_t>(vSegments + 1));
                            geometry.indices.push_back(vertexCounter + static_cast<uint32_t>(vSegments + 2));
                            geometry.indices.push_back(vertexCounter + 1u);
                        }
                        vertexCounter++;
                    }
                }
            }

            return withTangents(std::move(geometry));
        }

        PrimitiveGeometry createSphereGeometry()
        {
            PrimitiveGeometry geometry;

            constexpr float radius = 0.5f;
            constexpr int latitudeBands = 48;
            constexpr int longitudeBands = 48;

            for (int lat = 0; lat <= latitudeBands; ++lat) {
                const float theta = static_cast<float>(lat) * PI_F / static_cast<float>(latitudeBands);
                const float sinTheta = std::sin(theta);
                const float cosTheta = std::cos(theta);

                for (int lon = 0; lon <= longitudeBands; ++lon) {
                    const float phi = static_cast<float>(lon) * 2.0f * PI_F / static_cast<float>(longitudeBands) - PI_F * 0.5f;
                    const float sinPhi = std::sin(phi);
                    const float cosPhi = std::cos(phi);

                    const float x = cosPhi * sinTheta;
                    const float y = cosTheta;
                    const float z = sinPhi * sinTheta;
                    const float u = 1.0f - static_cast<float>(lon) / static_cast<float>(longitudeBands);
                    const float v = static_cast<float>(lat) / static_cast<float>(latitudeBands);

                    pushVertex(geometry, x * radius, y * radius, z * radius, x, y, z, u, v);
                }
            }

            for (int lat = 0; lat < latitudeBands; ++lat) {
                for (int lon = 0; lon < longitudeBands; ++lon) {
                    const uint32_t first = static_cast<uint32_t>(lat * (longitudeBands + 1) + lon);
                    const uint32_t second = first + static_cast<uint32_t>(longitudeBands + 1);
                    geometry.indices.push_back(first + 1u);
                    geometry.indices.push_back(second);
                    geometry.indices.push_back(first);
                    geometry.indices.push_back(first + 1u);
                    geometry.indices.push_back(second + 1u);
                    geometry.indices.push_back(second);
                }
            }

            return withTangents(std::move(geometry));
        }

        PrimitiveGeometry createConeBaseGeometry(const float baseRadius, const float peakRadius, const float height,
            const int heightSegments, const int capSegments, const bool roundedCaps)
        {
            PrimitiveGeometry geometry;

            if (height > 0.0f) {
                for (int i = 0; i <= heightSegments; ++i) {
                    for (int j = 0; j <= capSegments; ++j) {
                        const float theta = (static_cast<float>(j) / static_cast<float>(capSegments)) * 2.0f * PI_F - PI_F;
                        const float sinTheta = std::sin(theta);
                        const float cosTheta = std::cos(theta);

                        const Vector3 bottom(sinTheta * baseRadius, -height * 0.5f, cosTheta * baseRadius);
                        const Vector3 top(sinTheta * peakRadius, height * 0.5f, cosTheta * peakRadius);
                        const float t = static_cast<float>(i) / static_cast<float>(heightSegments);
                        const Vector3 pos = bottom + (top - bottom) * t;
                        const Vector3 bottomToTop = (top - bottom).normalized();
                        const Vector3 tangent(cosTheta, 0.0f, -sinTheta);
                        const Vector3 norm = tangent.cross(bottomToTop).normalized();

                        const float u = static_cast<float>(j) / static_cast<float>(capSegments);
                        const float v = 1.0f - static_cast<float>(i) / static_cast<float>(heightSegments);
                        pushVertex(geometry,
                            pos.getX(), pos.getY(), pos.getZ(),
                            norm.getX(), norm.getY(), norm.getZ(),
                            u, v);
                        // The body fills the first third, full height, with u and v
                        // swapped so the sweep runs down the long side of the cell.
                        pushUv1(geometry, 1.0f - v, u, 1.0f / 3.0f, 1.0f, 0.0f, 0.0f);

                        if (i < heightSegments && j < capSegments) {
                            const uint32_t first = static_cast<uint32_t>(i * (capSegments + 1) + j);
                            const uint32_t second = static_cast<uint32_t>(i * (capSegments + 1) + (j + 1));
                            const uint32_t third = static_cast<uint32_t>((i + 1) * (capSegments + 1) + j);
                            const uint32_t fourth = static_cast<uint32_t>((i + 1) * (capSegments + 1) + (j + 1));
                            geometry.indices.insert(geometry.indices.end(), {first, second, third});
                            geometry.indices.insert(geometry.indices.end(), {second, fourth, third});
                        }
                    }
                }
            }

            if (roundedCaps) {
                const int latitudeBands = std::max(1, capSegments / 2);
                const int longitudeBands = capSegments;
                const float capOffset = height * 0.5f;

                for (int lat = 0; lat <= latitudeBands; ++lat) {
                    const float theta = (static_cast<float>(lat) * PI_F * 0.5f) / static_cast<float>(latitudeBands);
                    const float sinTheta = std::sin(theta);
                    const float cosTheta = std::cos(theta);
                    for (int lon = 0; lon <= longitudeBands; ++lon) {
                        const float phi = static_cast<float>(lon) * 2.0f * PI_F / static_cast<float>(longitudeBands) - PI_F * 0.5f;
                        const float sinPhi = std::sin(phi);
                        const float cosPhi = std::cos(phi);
                        const float x = cosPhi * sinTheta;
                        const float y = cosTheta;
                        const float z = sinPhi * sinTheta;
                        const float u = 1.0f - static_cast<float>(lon) / static_cast<float>(longitudeBands);
                        const float v = static_cast<float>(lat) / static_cast<float>(latitudeBands);
                        pushVertex(geometry,
                            x * peakRadius, y * peakRadius + capOffset, z * peakRadius,
                            x, y, z,
                            u, v);
                        // Top cap in the second third.
                        pushUv1(geometry, u, 1.0f - v, 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f, 0.0f);
                    }
                }

                const uint32_t topOffset = static_cast<uint32_t>((heightSegments + 1) * (capSegments + 1));
                for (int lat = 0; lat < latitudeBands; ++lat) {
                    for (int lon = 0; lon < longitudeBands; ++lon) {
                        const uint32_t first = static_cast<uint32_t>(lat * (longitudeBands + 1) + lon);
                        const uint32_t second = first + static_cast<uint32_t>(longitudeBands + 1);
                        geometry.indices.insert(geometry.indices.end(), {
                            topOffset + first + 1u, topOffset + second, topOffset + first,
                            topOffset + first + 1u, topOffset + second + 1u, topOffset + second
                        });
                    }
                }

                for (int lat = 0; lat <= latitudeBands; ++lat) {
                    const float theta = PI_F * 0.5f + (static_cast<float>(lat) * PI_F * 0.5f) / static_cast<float>(latitudeBands);
                    const float sinTheta = std::sin(theta);
                    const float cosTheta = std::cos(theta);
                    for (int lon = 0; lon <= longitudeBands; ++lon) {
                        const float phi = static_cast<float>(lon) * 2.0f * PI_F / static_cast<float>(longitudeBands) - PI_F * 0.5f;
                        const float sinPhi = std::sin(phi);
                        const float cosPhi = std::cos(phi);
                        const float x = cosPhi * sinTheta;
                        const float y = cosTheta;
                        const float z = sinPhi * sinTheta;
                        const float u = 1.0f - static_cast<float>(lon) / static_cast<float>(longitudeBands);
                        const float v = static_cast<float>(lat) / static_cast<float>(latitudeBands);
                        pushVertex(geometry,
                            x * peakRadius, y * peakRadius - capOffset, z * peakRadius,
                            x, y, z,
                            u, v);
                        // Bottom cap in the third third.
                        pushUv1(geometry, u, 1.0f - v, 1.0f / 3.0f, 1.0f / 3.0f, 2.0f / 3.0f, 0.0f);
                    }
                }

                const uint32_t bottomOffset = static_cast<uint32_t>(
                    (heightSegments + 1) * (capSegments + 1) +
                    (longitudeBands + 1) * (latitudeBands + 1));
                for (int lat = 0; lat < latitudeBands; ++lat) {
                    for (int lon = 0; lon < longitudeBands; ++lon) {
                        const uint32_t first = static_cast<uint32_t>(lat * (longitudeBands + 1) + lon);
                        const uint32_t second = first + static_cast<uint32_t>(longitudeBands + 1);
                        geometry.indices.insert(geometry.indices.end(), {
                            bottomOffset + first + 1u, bottomOffset + second, bottomOffset + first,
                            bottomOffset + first + 1u, bottomOffset + second + 1u, bottomOffset + second
                        });
                    }
                }
            } else {
                uint32_t offset = static_cast<uint32_t>((heightSegments + 1) * (capSegments + 1));
                if (baseRadius > 0.0f) {
                    for (int i = 0; i < capSegments; ++i) {
                        const float theta = static_cast<float>(i) * 2.0f * PI_F / static_cast<float>(capSegments);
                        const float x = std::sin(theta);
                        const float z = std::cos(theta);
                        const float u = 1.0f - (x + 1.0f) * 0.5f;
                        const float v = 1.0f - (z + 1.0f) * 0.5f;
                        pushVertex(geometry,
                            x * baseRadius, -height * 0.5f, z * baseRadius,
                            0.0f, -1.0f, 0.0f,
                            u, v);
                        // Flat bottom cap in the second third.
                        pushUv1(geometry, u, 1.0f - v, 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f, 0.0f);
                        if (i > 1) {
                            geometry.indices.insert(geometry.indices.end(), {offset, offset + static_cast<uint32_t>(i), offset + static_cast<uint32_t>(i - 1)});
                        }
                    }
                }

                offset += static_cast<uint32_t>(capSegments);
                if (peakRadius > 0.0f) {
                    for (int i = 0; i < capSegments; ++i) {
                        const float theta = static_cast<float>(i) * 2.0f * PI_F / static_cast<float>(capSegments);
                        const float x = std::sin(theta);
                        const float z = std::cos(theta);
                        const float u = 1.0f - (x + 1.0f) * 0.5f;
                        const float v = 1.0f - (z + 1.0f) * 0.5f;
                        pushVertex(geometry,
                            x * peakRadius, height * 0.5f, z * peakRadius,
                            0.0f, 1.0f, 0.0f,
                            u, v);
                        // Flat top cap in the third third.
                        pushUv1(geometry, u, 1.0f - v, 1.0f / 3.0f, 1.0f / 3.0f, 2.0f / 3.0f, 0.0f);
                        if (i > 1) {
                            geometry.indices.insert(geometry.indices.end(), {offset, offset + static_cast<uint32_t>(i - 1), offset + static_cast<uint32_t>(i)});
                        }
                    }
                }
            }

            return withTangents(std::move(geometry));
        }

        PrimitiveGeometry createCylinderGeometry()
        {
            return createConeBaseGeometry(0.5f, 0.5f, 1.0f, 5, 20, false);
        }

        PrimitiveGeometry createConeGeometry()
        {
            return createConeBaseGeometry(0.5f, 0.0f, 1.0f, 5, 20, false);
        }

        PrimitiveGeometry createCapsuleGeometry()
        {
            // Upstream primitive cache defaults: radius=0.5, height=2, heightSegments=1, sides=20.
            return createConeBaseGeometry(0.5f, 0.5f, 1.0f, 1, 20, true);
        }

        PrimitiveGeometry createPlaneGeometry()
        {
            // createPlane: unit quad on XZ plane, Y up, centered at origin
            PrimitiveGeometry geometry;

            // 4 vertices: (-0.5, 0, -0.5) to (0.5, 0, 0.5)
            // Normal pointing up (+Y). UVs are upstream's: u = 0..1 along +X, v = 0 at
            // z = -0.5 and 1 at z = +0.5 (upstream writes `1 - v` with its v running from
            // +Z to -Z). The far edge samples the image's top row, which stands the
            // picture upright once the plane is rotated +90 degrees about X. The derived
            // tangent is +X (+u) and cross(n, t) * w is -Z, toward the image's top.
            pushVertex(geometry, -0.5f, 0.0f,  0.5f,  0.0f, 1.0f, 0.0f,  0.0f, 1.0f);
            pushVertex(geometry,  0.5f, 0.0f,  0.5f,  0.0f, 1.0f, 0.0f,  1.0f, 1.0f);
            pushVertex(geometry,  0.5f, 0.0f, -0.5f,  0.0f, 1.0f, 0.0f,  1.0f, 0.0f);
            pushVertex(geometry, -0.5f, 0.0f, -0.5f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f);

            // Two triangles
            geometry.indices = {0, 1, 2, 0, 2, 3};

            return withTangents(std::move(geometry));
        }

    RenderComponent::RenderComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity), _type("asset")
    {
        _instances.push_back(this);
    }

    RenderComponent::~RenderComponent()
    {
        // A batch still pointing at these mesh instances must go before they do.
        if (auto* batches = batcher(); batches && !_meshInstances.empty()) {
            batches->sourcesLeaving(_batchGroupId);
        }
        clearMeshInstances();
        _ownedMeshes.clear();

        _instances.erase(std::remove(_instances.begin(), _instances.end(), this), _instances.end());
    }

    const std::vector<MeshInstance*>& RenderComponent::meshInstances() const
    {
        rebuildMeshInstanceView();
        return _meshInstanceView;
    }

    MeshInstance* RenderComponent::addMeshInstance(std::unique_ptr<MeshInstance> meshInstance)
    {
        if (!meshInstance) {
            return nullptr;
        }

        auto* meshInstanceRaw = meshInstance.get();
        _meshInstances.push_back(std::move(meshInstance));
        _meshInstanceViewDirty = true;
        // A new source for the group (upstream's batcher insert).
        if (auto* batches = batcher(); batches && active()) {
            batches->markGroupDirty(_batchGroupId);
        }
        return meshInstanceRaw;
    }

    BatchManager* RenderComponent::batcher() const
    {
        if (_batchGroupId < 0 || entity() == nullptr || entity()->engine() == nullptr) {
            return nullptr;
        }
        return entity()->engine()->batcher();
    }

    void RenderComponent::setBatchGroupId(const int id)
    {
        if (id == _batchGroupId) {
            return;
        }
        if (auto* batches = batcher(); batches && active()) {
            batches->sourcesLeaving(_batchGroupId);
        }
        _batchGroupId = id;
        for (const auto& mi : _meshInstances) {
            mi->setBatchGroupId(id);
        }
        if (auto* batches = batcher(); batches && active()) {
            batches->markGroupDirty(_batchGroupId);
        }
    }

    // Upstream's render component inserts into and removes from the batcher on
    // enable and disable. Disabling is also what an entity's destroy() does first.
    void RenderComponent::onEnable()
    {
        if (auto* batches = batcher()) {
            batches->markGroupDirty(_batchGroupId);
        }
    }

    void RenderComponent::onDisable()
    {
        if (auto* batches = batcher()) {
            batches->sourcesLeaving(_batchGroupId);
        }
    }

    void RenderComponent::clearMeshInstances()
    {
        if (auto* batches = batcher(); batches && !_meshInstances.empty()) {
            batches->sourcesLeaving(_batchGroupId);
        }
        _meshInstances.clear();
        _meshInstanceView.clear();
        _meshInstanceViewDirty = false;
    }

    void RenderComponent::setType(const std::string& type)
    {
        if (_type == type) {
            return;
        }
        _type = type;
        rebuildPrimitiveMesh();
    }

    void RenderComponent::setMaterial(Material* material)
    {
        if (_material == material) {
            return;
        }
        _material = material;

        if (_type != "asset") {
            rebuildPrimitiveMesh();
        }
    }

    void RenderComponent::setReceiveShadows(const bool value)
    {
        _receiveShadows = value;
        //set receiveShadows() propagates to all mesh instances.
        for (const auto& mi : _meshInstances) {
            if (mi) {
                mi->setReceiveShadow(value);
            }
        }
    }

    void RenderComponent::setCastShadows(const bool value)
    {
        _castShadows = value;
        //set castShadows() propagates to all mesh instances.
        for (const auto& mi : _meshInstances) {
            if (mi) {
                mi->setCastShadow(value);
            }
        }
    }

    void RenderComponent::cloneFrom(const Component* source)
    {
        const auto* src = dynamic_cast<const RenderComponent*>(source);
        if (!src) {
            return;
        }

        // Fields, not setters: setType / setMaterial would build a SECOND primitive
        // mesh when the source's can be shared.
        _type = src->_type;
        _layers = src->_layers;
        _material = src->_material;
        _receiveShadows = src->_receiveShadows;
        _castShadows = src->_castShadows;
        _batchGroupId = src->_batchGroupId;
        setEnabled(src->enabled());

        // The mesh instances share the source's meshes and materials (upstream
        // `_onSetMeshes(meshes)` plus the material copy). A primitive's mesh is owned
        // by the component, so the clone co-owns it: it has to outlive the source.
        //
        // An instance another owner attached here — a splat (GSplatComponent), an
        // emitter (ParticleSystemComponent), a storage draw (WideLineRenderer) — is
        // NOT copied: its owner builds the clone's own, and may already have done so
        // on this component, which is why nothing is cleared first.
        _ownedMeshes = src->_ownedMeshes;
        for (const auto& srcMi : src->_meshInstances) {
            if (!srcMi || srcMi->gsplatInstance() || srcMi->particleEmitter() ||
                srcMi->storageDrawCount() > 0) {
                continue;
            }
            addMeshInstance(srcMi->cloneFor(_entity));
        }
    }

    void RenderComponent::resolveClonedReferences(const Component* /*source*/, const CloneNodeMap& map)
    {
        // A skinned mesh's bones are nodes of the model it came with; the clone must
        // be driven by the CLONED skeleton (upstream remaps `rootBone`, from which it
        // rebuilds the skin). A bone outside the cloned subtree stays shared.
        for (const auto& mi : _meshInstances) {
            auto* skin = mi ? mi->skinInstance() : nullptr;
            if (!skin) {
                continue;
            }
            std::vector<GraphNode*> bones = skin->bones();
            for (auto& bone : bones) {
                bone = remapCloned(bone, map);
            }
            skin->setBones(std::move(bones));
            skin->setRootBone(remapCloned(skin->rootBone(), map));
        }
    }

    void RenderComponent::rebuildPrimitiveMesh()
    {
        clearMeshInstances();
        _ownedMeshes.clear();

        if (_type == "asset") {
            return;
        }

        auto* owner = entity();
        auto* ownerEngine = owner ? owner->engine() : nullptr;
        const auto device = ownerEngine ? ownerEngine->graphicsDevice() : nullptr;
        if (!device) {
            return;
        }

        PrimitiveGeometry primitiveGeometry;
        if (_type == "box") {
            primitiveGeometry = createBoxGeometry();
        } else if (_type == "sphere") {
            primitiveGeometry = createSphereGeometry();
        } else if (_type == "cylinder") {
            primitiveGeometry = createCylinderGeometry();
        } else if (_type == "cone") {
            primitiveGeometry = createConeGeometry();
        } else if (_type == "capsule") {
            primitiveGeometry = createCapsuleGeometry();
        } else if (_type == "plane") {
            primitiveGeometry = createPlaneGeometry();
        } else {
            // DEVIATION: Current C++ RenderComponent primitive port implements box/sphere/cylinder/cone/capsule/plane only.
            spdlog::warn("Unsupported render primitive type '{}'", _type);
            return;
        }

        auto mesh = createMesh(device, primitiveGeometry);
        if (!mesh) {
            return;
        }

        _ownedMeshes.push_back(mesh);
        auto meshInstance = std::make_unique<MeshInstance>(mesh.get(), _material, owner);
        meshInstance->setReceiveShadow(_receiveShadows);
        meshInstance->setCastShadow(_castShadows);
        meshInstance->setBatchGroupId(_batchGroupId);
        addMeshInstance(std::move(meshInstance));
    }

    void RenderComponent::rebuildMeshInstanceView() const
    {
        if (!_meshInstanceViewDirty) {
            return;
        }

        _meshInstanceView.clear();
        _meshInstanceView.reserve(_meshInstances.size());
        for (const auto& meshInstance : _meshInstances) {
            _meshInstanceView.push_back(meshInstance.get());
        }
        _meshInstanceViewDirty = false;
    }
}
