// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis  on 09.10.2025.
//

#include "skyMesh.h"

#include <cmath>
#include <cstring>

#include "spdlog/spdlog.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/materials/material.h"
#include "scene/scene.h"

namespace visutwin::canvas
{
    namespace
    {
        std::shared_ptr<Mesh> createMesh(const std::shared_ptr<GraphicsDevice>& device,
            const std::vector<float>& vertices, const std::vector<uint32_t>& indices)
        {
            if (!device) {
                return nullptr;
            }

            auto mesh = std::make_shared<Mesh>();

            std::vector<uint8_t> vertexBytes(vertices.size() * sizeof(float));
            std::memcpy(vertexBytes.data(), vertices.data(), vertexBytes.size());
            VertexBufferOptions vbOptions;
            vbOptions.data = std::move(vertexBytes);
            auto vertexFormat = std::make_shared<VertexFormat>(14 * static_cast<int>(sizeof(float)), true, false);
            auto vertexBuffer = device->createVertexBuffer(vertexFormat, static_cast<int>(vertices.size() / 14), vbOptions);

            std::vector<uint8_t> indexBytes(indices.size() * sizeof(uint32_t));
            std::memcpy(indexBytes.data(), indices.data(), indexBytes.size());
            auto indexBuffer = device->createIndexBuffer(INDEXFORMAT_UINT32, static_cast<int>(indices.size()), indexBytes);

            Primitive primitive;
            primitive.type = PRIMITIVE_TRIANGLES;
            primitive.base = 0;
            primitive.baseVertex = 0;
            primitive.count = static_cast<int>(indices.size());
            primitive.indexed = true;

            mesh->setVertexBuffer(vertexBuffer);
            mesh->setIndexBuffer(indexBuffer, 0);
            mesh->setPrimitive(primitive, 0);

            BoundingBox bounds;
            bounds.setCenter(0.0f, 0.0f, 0.0f);
            bounds.setHalfExtents(1.0f, 1.0f, 1.0f);
            mesh->setAabb(bounds);

            return mesh;
        }

        std::vector<float> appendVertex(float x, float y, float z)
        {
            // position(3), normal(3), uv0(2), tangent(4), uv1(2)
            return {
                x, y, z,
                0.0f, 1.0f, 0.0f,
                0.0f, 0.0f,
                1.0f, 0.0f, 0.0f, 1.0f,
                0.0f, 0.0f
            };
        }

        std::shared_ptr<Mesh> createSkyBoxMesh(const std::shared_ptr<GraphicsDevice>& device, const float yOffset)
        {
            const float he = 1.0f;
            const float minY = -he + yOffset;
            const float maxY = he + yOffset;

            struct Vec3f
            {
                float x;
                float y;
                float z;
            };

            const Vec3f corners[8] = {
                {-he, minY,  he},
                { he, minY,  he},
                { he, maxY,  he},
                {-he, maxY,  he},
                { he, minY, -he},
                {-he, minY, -he},
                {-he, maxY, -he},
                { he, maxY, -he}
            };

            const int faceAxes[6][3] = {
                {0, 1, 3}, // FRONT
                {4, 5, 7}, // BACK
                {3, 2, 6}, // TOP
                {1, 0, 4}, // BOTTOM
                {1, 4, 2}, // RIGHT
                {5, 0, 6}  // LEFT
            };

            const float faceNormals[6][3] = {
                { 0.0f,  0.0f,  1.0f}, // FRONT
                { 0.0f,  0.0f, -1.0f}, // BACK
                { 0.0f,  1.0f,  0.0f}, // TOP
                { 0.0f, -1.0f,  0.0f}, // BOTTOM
                { 1.0f,  0.0f,  0.0f}, // RIGHT
                {-1.0f,  0.0f,  0.0f}  // LEFT
            };

            std::vector<float> vertices;
            std::vector<uint32_t> indices;
            vertices.reserve(6u * 4u * 14u);
            indices.reserve(6u * 6u);

            uint32_t vcounter = 0;
            constexpr int uSegments = 1;
            constexpr int vSegments = 1;

            for (int side = 0; side < 6; ++side) {
                for (int i = 0; i <= uSegments; ++i) {
                    for (int j = 0; j <= vSegments; ++j) {
                        const float u = static_cast<float>(i) / static_cast<float>(uSegments);
                        const float v = static_cast<float>(j) / static_cast<float>(vSegments);

                        const Vec3f c0 = corners[faceAxes[side][0]];
                        const Vec3f c1 = corners[faceAxes[side][1]];
                        const Vec3f c2 = corners[faceAxes[side][2]];

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
                        const Vec3f pos = {
                            temp1.x + (temp2.x - c0.x),
                            temp1.y + (temp2.y - c0.y),
                            temp1.z + (temp2.z - c0.z)
                        };

                        vertices.insert(vertices.end(), {
                            pos.x, pos.y, pos.z,
                            faceNormals[side][0], faceNormals[side][1], faceNormals[side][2],
                            u, 1.0f - v,
                            1.0f, 0.0f, 0.0f, 1.0f,
                            0.0f, 0.0f
                        });

                        if (i < uSegments && j < vSegments) {
                            indices.push_back(vcounter + static_cast<uint32_t>(vSegments + 1));
                            indices.push_back(vcounter + 1u);
                            indices.push_back(vcounter);
                            indices.push_back(vcounter + static_cast<uint32_t>(vSegments + 1));
                            indices.push_back(vcounter + static_cast<uint32_t>(vSegments + 2));
                            indices.push_back(vcounter + 1u);
                        }
                        vcounter++;
                    }
                }
            }

            return createMesh(device, vertices, indices);
        }
    }

    SkyMesh::SkyMesh(const std::shared_ptr<GraphicsDevice>& device, Scene* scene,
        GraphNode* node, Texture* texture, const int type)
        : _scene(scene)
    {
        (void)texture;
        if (!device || !_scene || !node) {
            spdlog::warn("SkyMesh: invalid args (device={}, scene={}, node={})", (void*)device.get(), (void*)_scene, (void*)node);
            return;
        }

        _mesh = createMeshByType(device, type);
        if (!_mesh) {
            spdlog::warn("SkyMesh: createMeshByType returned null for type={}", type);
            return;
        }

        auto material = std::make_shared<Material>();
        material->setName("SkyMaterial");
        material->setIsSkybox(true);
        // render inside of sky geometry.
        material->setCullMode(CullMode::CULLFACE_FRONT);
        // Skybox must not write to the depth buffer.
        // Without this, the skybox (rendered at depth ~0.9999) overwrites distant scene geometry
        // whose depth values exceed 0.9999 due to perspective depth non-linearity.
        auto skyDepthState = std::make_shared<DepthState>();
        skyDepthState->setDepthWrite(false);
        material->setDepthState(skyDepthState);
        _material = material;

        _meshInstance = std::make_unique<MeshInstance>(_mesh.get(), _material.get(), node);

        auto layers = _scene->layers();
        if (!layers) {
            spdlog::warn("SkyMesh: scene has no layers — cannot add sky mesh instance to skybox layer");
            return;
        }

        auto skyLayer = layers->getLayerById(LAYERID_SKYBOX);
        if (skyLayer) {
            skyLayer->addMeshInstances({_meshInstance.get()});
        } else {
            spdlog::warn("SkyMesh: skybox layer (LAYERID_SKYBOX={}) not found", LAYERID_SKYBOX);
        }
    }

    SkyMesh::~SkyMesh()
    {
        if (_scene && _meshInstance) {
            auto layers = _scene->layers();
            if (layers) {
                auto skyLayer = layers->getLayerById(LAYERID_SKYBOX);
                if (skyLayer) {
                    skyLayer->removeMeshInstances({_meshInstance.get()});
                }
            }
        }
    }

    std::shared_ptr<Mesh> SkyMesh::createInfiniteMesh(const std::shared_ptr<GraphicsDevice>& device) const
    {
        return createSkyBoxMesh(device, 0.0f);
    }

    std::shared_ptr<Mesh> SkyMesh::createBoxMesh(const std::shared_ptr<GraphicsDevice>& device) const
    {
        // SKYTYPE_BOX uses yOffset: 0.5.
        return createSkyBoxMesh(device, 0.5f);
    }

    std::shared_ptr<Mesh> SkyMesh::createDomeMesh(const std::shared_ptr<GraphicsDevice>& device) const
    {
        // DomeGeometry extends SphereGeometry.
        // Full sphere with 50x50 bands, then bottom hemisphere is flattened.
        constexpr int latitudeBands = 50;
        constexpr int longitudeBands = 50;
        constexpr float pi = 3.14159265358979323846f;
        constexpr float radius = 0.5f;
        constexpr float bottomLimit = 0.1f;
        constexpr float curvatureRadius = 0.95f;
        constexpr float curvatureRadiusSq = curvatureRadius * curvatureRadius;

        // Step 1: Generate full sphere vertices (SphereGeometry)
        std::vector<float> positions;
        positions.reserve((latitudeBands + 1) * (longitudeBands + 1) * 3);

        for (int lat = 0; lat <= latitudeBands; ++lat) {
            const float theta = static_cast<float>(lat) * pi / static_cast<float>(latitudeBands);
            const float sinTheta = std::sin(theta);
            const float cosTheta = std::cos(theta);

            for (int lon = 0; lon <= longitudeBands; ++lon) {
                // sweep from +Z axis (offset by -pi/2)
                const float phi = static_cast<float>(lon) * 2.0f * pi / static_cast<float>(longitudeBands) - pi * 0.5f;
                const float sinPhi = std::sin(phi);
                const float cosPhi = std::cos(phi);

                const float x = cosPhi * sinTheta;
                const float y = cosTheta;
                const float z = sinPhi * sinTheta;

                positions.push_back(x * radius);
                positions.push_back(y * radius);
                positions.push_back(z * radius);
            }
        }

        // Step 2: Flatten bottom hemisphere (DomeGeometry post-processing)
        for (size_t i = 0; i < positions.size(); i += 3) {
            const float x = positions[i] / radius;
            float y = positions[i + 1] / radius;
            const float z = positions[i + 2] / radius;

            if (y < 0.0f) {
                // Scale vertices on the bottom
                y *= 0.3f;

                // Flatten the center
                if (x * x + z * z < curvatureRadiusSq) {
                    y = -bottomLimit;
                }
            }

            // Adjust y to have the center at the flat bottom
            y += bottomLimit;
            positions[i + 1] = y * radius;
        }

        // Step 3: Pack into 14-float vertex format
        std::vector<float> vertices;
        vertices.reserve((latitudeBands + 1) * (longitudeBands + 1) * 14);

        for (size_t i = 0; i < positions.size(); i += 3) {
            const auto packed = appendVertex(positions[i], positions[i + 1], positions[i + 2]);
            vertices.insert(vertices.end(), packed.begin(), packed.end());
        }

        // Step 4: Generate indices — Standard winding order
        std::vector<uint32_t> indices;
        indices.reserve(latitudeBands * longitudeBands * 6);

        for (int lat = 0; lat < latitudeBands; ++lat) {
            for (int lon = 0; lon < longitudeBands; ++lon) {
                const uint32_t first = static_cast<uint32_t>(lat * (longitudeBands + 1) + lon);
                const uint32_t second = first + static_cast<uint32_t>(longitudeBands + 1);

                // (first+1, second, first) and (first+1, second+1, second)
                indices.push_back(first + 1);
                indices.push_back(second);
                indices.push_back(first);
                indices.push_back(first + 1);
                indices.push_back(second + 1);
                indices.push_back(second);
            }
        }

        return createMesh(device, vertices, indices);
    }

    std::shared_ptr<Mesh> SkyMesh::createSphereMesh(const std::shared_ptr<GraphicsDevice>& device,
        const int latBands, const int lonBands)
    {
        constexpr float pi = 3.14159265358979323846f;
        // Large radius ensures the sphere extends well beyond the near clip plane.
        // At globe scale, near clip can be 20+ km; radius must exceed that.
        // clip.z is overridden to far plane in the vertex shader, so the actual
        // radius only affects near-plane clipping, not perceived depth.
        // viewDir = normalize(v.position) is scale-invariant.
        constexpr float radius = 500000.0f;  // 500 km

        std::vector<float> vertices;
        vertices.reserve((latBands + 1) * (lonBands + 1) * 14);

        for (int lat = 0; lat <= latBands; ++lat) {
            const float theta = static_cast<float>(lat) * pi / static_cast<float>(latBands);
            const float sinTheta = std::sin(theta);
            const float cosTheta = std::cos(theta);

            for (int lon = 0; lon <= lonBands; ++lon) {
                const float phi = static_cast<float>(lon) * 2.0f * pi / static_cast<float>(lonBands);
                const float sinPhi = std::sin(phi);
                const float cosPhi = std::cos(phi);

                const float x = cosPhi * sinTheta * radius;
                const float y = cosTheta * radius;
                const float z = sinPhi * sinTheta * radius;

                const auto packed = appendVertex(x, y, z);
                vertices.insert(vertices.end(), packed.begin(), packed.end());
            }
        }

        std::vector<uint32_t> indices;
        indices.reserve(latBands * lonBands * 6);

        for (int lat = 0; lat < latBands; ++lat) {
            for (int lon = 0; lon < lonBands; ++lon) {
                const uint32_t first = static_cast<uint32_t>(lat * (lonBands + 1) + lon);
                const uint32_t second = first + static_cast<uint32_t>(lonBands + 1);

                indices.push_back(first + 1);
                indices.push_back(second);
                indices.push_back(first);
                indices.push_back(first + 1);
                indices.push_back(second + 1);
                indices.push_back(second);
            }
        }

        return createMesh(device, vertices, indices);
    }

    std::shared_ptr<Mesh> SkyMesh::createMeshByType(const std::shared_ptr<GraphicsDevice>& device, const int type) const
    {
        switch (type) {
        case SKYTYPE_BOX:
            return createBoxMesh(device);
        case SKYTYPE_DOME:
            return createDomeMesh(device);
        case SKYTYPE_ATMOSPHERE:
            return createSphereMesh(device);
        case SKYTYPE_INFINITE:
        default:
            // Use a sphere mesh instead of a cube for the infinite skybox.
            // The cube mesh has face edges where the 16× anisotropic filter
            // sees derivative discontinuities, creating visible vertical lines
            // in the equirectangular env atlas sampling.  A sphere has no face
            // edges, so the UV gradient is continuous everywhere.
            return createSphereMesh(device, 64, 64);
        }
    }
} // visutwin
