// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.02.2026.
//
#include "renderComponent.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include <spdlog/spdlog.h>

#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/vertexFormat.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr float PI_F = 3.14159265358979323846f;

        struct PrimitiveGeometry
        {
            std::vector<float> positions;
            std::vector<float> normals;
            std::vector<float> uvs;
            std::vector<float> tangents;
            std::vector<uint32_t> indices;
        };

        void pushVertex(PrimitiveGeometry& geometry,
            const float px, const float py, const float pz,
            const float nx, const float ny, const float nz,
            const float tx, const float ty, const float tz, const float tw,
            const float u, const float v)
        {
            geometry.positions.insert(geometry.positions.end(), {px, py, pz});
            geometry.normals.insert(geometry.normals.end(), {nx, ny, nz});
            geometry.tangents.insert(geometry.tangents.end(), {tx, ty, tz, tw});
            geometry.uvs.insert(geometry.uvs.end(), {u, v});
        }

        std::shared_ptr<Mesh> createMesh(const std::shared_ptr<GraphicsDevice>& device, const PrimitiveGeometry& geometry)
        {
            if (!device || geometry.positions.empty() || geometry.indices.empty()) {
                return nullptr;
            }

            const int vertexCount = static_cast<int>(geometry.positions.size() / 3);
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

                minBounds = Vector3(
                    std::min(minBounds.getX(), px),
                    std::min(minBounds.getY(), py),
                    std::min(minBounds.getZ(), pz));
                maxBounds = Vector3(
                    std::max(maxBounds.getX(), px),
                    std::max(maxBounds.getY(), py),
                    std::max(maxBounds.getZ(), pz));

                interleaved.insert(interleaved.end(), {
                    px, py, pz,
                    nx, ny, nz,
                    u, v,
                    tx, ty, tz, tw,
                    // DEVIATION: Primitive mesh path currently mirrors UV0 into UV1.
                    u, v
                });
            }

            std::vector<uint8_t> vertexBytes(interleaved.size() * sizeof(float));
            std::memcpy(vertexBytes.data(), interleaved.data(), vertexBytes.size());
            VertexBufferOptions vbOptions;
            vbOptions.data = std::move(vertexBytes);

            auto vertexFormat = std::make_shared<VertexFormat>(14 * static_cast<int>(sizeof(float)), true, false);
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

        PrimitiveGeometry createBoxGeometry()
        {
            PrimitiveGeometry geometry;

            constexpr float halfExtent = 0.5f;
            constexpr int uSegments = 1;
            constexpr int vSegments = 1;

            struct Vec3f
            {
                float x;
                float y;
                float z;
            };

            const std::array<Vec3f, 8> corners = {{
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

                        const Vec3f temp1 = {
                            c0.x + (c1.x - c0.x) * u,
                            c0.y + (c1.y - c0.y) * u,
                            c0.z + (c1.z - c0.z) * u
                        };
                        const Vec3f temp2 = {
                            c0.x + (c2.x - c0.x) * v,
                            c0.y + (c2.y - c0.y) * v,
                            c0.z + (c2.z - c0.z) * v
                        };

                        pushVertex(
                            geometry,
                            temp1.x + (temp2.x - c0.x),
                            temp1.y + (temp2.y - c0.y),
                            temp1.z + (temp2.z - c0.z),
                            faceNormals[side][0], faceNormals[side][1], faceNormals[side][2],
                            1.0f, 0.0f, 0.0f, 1.0f,
                            u, 1.0f - v);

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

            return geometry;
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

                    const float tx = -sinPhi;
                    const float ty = 0.0f;
                    const float tz = cosPhi;
                    pushVertex(geometry, x * radius, y * radius, z * radius, x, y, z, tx, ty, tz, 1.0f, u, v);
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

            return geometry;
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
                            tangent.getX(), tangent.getY(), tangent.getZ(), 1.0f,
                            u, v);

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
                            -sinPhi, 0.0f, cosPhi, 1.0f,
                            u, v);
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
                            -sinPhi, 0.0f, cosPhi, 1.0f,
                            u, v);
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
                            1.0f, 0.0f, 0.0f, 1.0f,
                            u, v);
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
                            1.0f, 0.0f, 0.0f, 1.0f,
                            u, v);
                        if (i > 1) {
                            geometry.indices.insert(geometry.indices.end(), {offset, offset + static_cast<uint32_t>(i - 1), offset + static_cast<uint32_t>(i)});
                        }
                    }
                }
            }

            return geometry;
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
            // Normal pointing up (+Y), tangent along +X
            pushVertex(geometry, -0.5f, 0.0f,  0.5f,  0.0f, 1.0f, 0.0f,  1.0f, 0.0f, 0.0f, 1.0f,  0.0f, 0.0f);
            pushVertex(geometry,  0.5f, 0.0f,  0.5f,  0.0f, 1.0f, 0.0f,  1.0f, 0.0f, 0.0f, 1.0f,  1.0f, 0.0f);
            pushVertex(geometry,  0.5f, 0.0f, -0.5f,  0.0f, 1.0f, 0.0f,  1.0f, 0.0f, 0.0f, 1.0f,  1.0f, 1.0f);
            pushVertex(geometry, -0.5f, 0.0f, -0.5f,  0.0f, 1.0f, 0.0f,  1.0f, 0.0f, 0.0f, 1.0f,  0.0f, 1.0f);

            // Two triangles
            geometry.indices = {0, 1, 2, 0, 2, 3};

            return geometry;
        }
    }

    RenderComponent::RenderComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity), _type("asset")
    {
        _instances.push_back(this);
    }

    RenderComponent::~RenderComponent()
    {
        for (auto* meshInstance : _meshInstances) {
            delete meshInstance;
        }
        _meshInstances.clear();
        _ownedMeshes.clear();

        _instances.erase(std::remove(_instances.begin(), _instances.end(), this), _instances.end());
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
        for (auto* mi : _meshInstances) {
            if (mi) {
                mi->setReceiveShadow(value);
            }
        }
    }

    void RenderComponent::setCastShadows(const bool value)
    {
        _castShadows = value;
        //set castShadows() propagates to all mesh instances.
        for (auto* mi : _meshInstances) {
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

        //
        // Copy scalar properties.
        _type = src->_type;
        _layers = src->_layers;
        _material = src->_material;
        _receiveShadows = src->_receiveShadows;
        _castShadows = src->_castShadows;
        setEnabled(src->enabled());

        // Clone mesh instances: create new MeshInstance per source, sharing mesh and material,
        // but pointing to this component's entity (the cloned node).
        for (auto* mi : _meshInstances) {
            delete mi;
        }
        _meshInstances.clear();

        for (const auto* srcMi : src->_meshInstances) {
            if (!srcMi) {
                continue;
            }
            auto* clonedMi = new MeshInstance(srcMi->mesh(), srcMi->material(), _entity);
            clonedMi->setCastShadow(srcMi->castShadow());
            clonedMi->setReceiveShadow(srcMi->receiveShadow());
            clonedMi->setCull(srcMi->cull());
            clonedMi->setMask(srcMi->mask());
            _meshInstances.push_back(clonedMi);
        }
    }

    void RenderComponent::rebuildPrimitiveMesh()
    {
        for (auto* meshInstance : _meshInstances) {
            delete meshInstance;
        }
        _meshInstances.clear();
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
        _meshInstances.push_back(new MeshInstance(mesh.get(), _material, owner));
    }
}
