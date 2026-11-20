// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 20.11.2026
//
// The built-in primitives' CPU geometry and the mesh built from it, declared in
// primitiveGeometry.h (moved out of renderComponent.cpp, which keeps only the component).
//
#include "primitiveGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <utility>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/geometry/geometryUtils.h"
#include "scene/mesh.h"

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

        // Every lightmap cell keeps an 8/64 border of
        // its own size free, so bilinear taps and the bake's dilation stay inside it.
        constexpr float kUv1Padding = 8.0f / 64.0f;
        constexpr float kUv1PaddingScale = 1.0f - kUv1Padding * 2.0f;

        // One UV1. (u, v) are UNFLIPPED coordinates in
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
    }

    GeometryVertexBuffer createGeometryVertexBuffer(const std::shared_ptr<GraphicsDevice>& device,
        const PrimitiveGeometry& geometry)
    {
        GeometryVertexBuffer result;
        if (!device || geometry.positions.empty()) {
            return result;
        }

        const int vertexCount = static_cast<int>(geometry.positions.size() / 3);
        // A primitive without an unwrap of its own (the plane and sphere) uses its UV0
        // as UV1.
        const std::vector<float>& uvs1 =
            geometry.uvs1.size() == geometry.uvs.size() ? geometry.uvs1 : geometry.uvs;
        const bool hasTangents = geometry.tangents.size() >= static_cast<size_t>(vertexCount) * 4u;
        std::vector<float> interleaved;
        interleaved.reserve(static_cast<size_t>(vertexCount) * 14u);

        Vector3 minBounds(std::numeric_limits<float>::max());
        Vector3 maxBounds(std::numeric_limits<float>::lowest());

        for (int i = 0; i < vertexCount; ++i) {
            const size_t posOffset = static_cast<size_t>(i) * 3u;
            const size_t uvOffset = static_cast<size_t>(i) * 2u;
            const size_t tanOffset = static_cast<size_t>(i) * 4u;

            const float px = geometry.positions[posOffset];
            const float py = geometry.positions[posOffset + 1u];
            const float pz = geometry.positions[posOffset + 2u];
            const float tx = hasTangents ? geometry.tangents[tanOffset] : 0.0f;
            const float ty = hasTangents ? geometry.tangents[tanOffset + 1u] : 0.0f;
            const float tz = hasTangents ? geometry.tangents[tanOffset + 2u] : 0.0f;
            const float tw = hasTangents ? geometry.tangents[tanOffset + 3u] : 1.0f;

            const Vector3 position(px, py, pz);
            minBounds = Vector3::min(minBounds, position);
            maxBounds = Vector3::max(maxBounds, position);

            interleaved.insert(interleaved.end(), {
                px, py, pz,
                geometry.normals[posOffset], geometry.normals[posOffset + 1u], geometry.normals[posOffset + 2u],
                geometry.uvs[uvOffset], geometry.uvs[uvOffset + 1u],
                tx, ty, tz, tw,
                uvs1[uvOffset], uvs1[uvOffset + 1u]
            });
        }

        std::vector<uint8_t> vertexBytes(interleaved.size() * sizeof(float));
        std::memcpy(vertexBytes.data(), interleaved.data(), vertexBytes.size());
        VertexBufferOptions vbOptions;
        vbOptions.data = std::move(vertexBytes);

        auto vertexFormat = std::make_shared<VertexFormat>(
            14 * static_cast<int>(sizeof(float)), VertexFormat::standardElements(), true, false);
        result.vertexBuffer = device->createVertexBuffer(vertexFormat, vertexCount, vbOptions);
        result.aabb.setCenter((minBounds + maxBounds) * 0.5f);
        result.aabb.setHalfExtents((maxBounds - minBounds) * 0.5f);
        return result;
    }

    std::shared_ptr<Mesh> createMeshFromGeometry(const std::shared_ptr<GraphicsDevice>& device,
        const PrimitiveGeometry& geometry)
    {
        if (!device || geometry.positions.empty() || geometry.indices.empty()) {
            return nullptr;
        }
        GeometryVertexBuffer vertices = createGeometryVertexBuffer(device, geometry);

        std::vector<uint8_t> indexBytes(geometry.indices.size() * sizeof(uint32_t));
        std::memcpy(indexBytes.data(), geometry.indices.data(), indexBytes.size());
        auto indexBuffer = device->createIndexBuffer(
            INDEXFORMAT_UINT32,
            static_cast<int>(geometry.indices.size()),
            indexBytes);

        auto mesh = std::make_shared<Mesh>();
        mesh->setVertexBuffer(std::move(vertices.vertexBuffer));
        mesh->setIndexBuffer(indexBuffer, 0);

        Primitive primitive;
        primitive.type = PRIMITIVE_TRIANGLES;
        primitive.base = 0;
        primitive.baseVertex = 0;
        primitive.count = static_cast<int>(geometry.indices.size());
        primitive.indexed = true;
        mesh->setPrimitive(primitive, 0);
        mesh->setAabb(vertices.aabb);
        return mesh;
    }

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
                        // The six faces pack 3x2, one face per cell (the
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

        namespace
        {
            struct AxisSamples
            {
                std::vector<double> coords;     // position on the box before the rounding
                std::vector<double> fractions;  // distance along the rounded surface / total
            };

            // Samples one axis of a face from -halfExtent to +halfExtent: each rounded end covers
            // the 45 degrees of arc up to the seam with the neighbouring face at equal angles, the
            // flat part between them evenly.
            AxisSamples sampleRoundedAxis(const double halfExtent, const double radius, const int flatSegments,
                const int roundSegments)
            {
                const double inner = halfExtent - radius;
                const int flat = inner > 0.0 ? flatSegments : 0;
                const double arc = radius * std::numbers::pi / 4.0;
                const double length = 2.0 * (inner + arc);
                const double invLength = length > 0.0 ? 1.0 / length : 0.0;

                std::vector<double> roundCoords;
                std::vector<double> arcs;
                for (int i = 1; i <= roundSegments; ++i) {
                    const double angle = static_cast<double>(i) / roundSegments * std::numbers::pi / 4.0;
                    // The last sample sits exactly on the box's edge, so neighbouring faces build
                    // bit-identical vertices along the seam.
                    roundCoords.push_back(i == roundSegments ? halfExtent : inner + radius * std::tan(angle));
                    arcs.push_back(radius * angle);
                }

                AxisSamples samples;
                for (int i = roundSegments - 1; i >= 0; --i) {
                    samples.coords.push_back(-roundCoords[static_cast<size_t>(i)]);
                    samples.fractions.push_back((arc - arcs[static_cast<size_t>(i)]) * invLength);
                }
                for (int i = 0; i <= flat; ++i) {
                    const double f = flat > 0 ? static_cast<double>(i) / flat : 0.0;
                    samples.coords.push_back(-inner + 2.0 * inner * f);
                    samples.fractions.push_back((arc + 2.0 * inner * f) * invLength);
                }
                for (int i = 0; i < roundSegments; ++i) {
                    samples.coords.push_back(roundCoords[static_cast<size_t>(i)]);
                    samples.fractions.push_back((arc + 2.0 * inner + arcs[static_cast<size_t>(i)]) * invLength);
                }
                return samples;
            }
        }

        PrimitiveGeometry createRoundedBoxGeometry(const RoundedBoxGeometryOptions& options)
        {
            // The box's faces in its order and orientation: the axis and sign of the normal, of u
            // and of v.
            struct Face { int n, ns, u, us, v, vs; };
            constexpr std::array<Face, 6> faces = {{
                {2, 1, 0, 1, 1, 1},      // front
                {2, -1, 0, -1, 1, 1},    // back
                {1, 1, 0, 1, 2, -1},     // top
                {1, -1, 0, -1, 2, -1},   // bottom
                {0, 1, 2, -1, 1, 1},     // right
                {0, -1, 2, 1, 1, 1}      // left
            }};

            const double he[3] = {options.halfExtents[0], options.halfExtents[1], options.halfExtents[2]};
            const double radius = std::max(0.0, std::min({static_cast<double>(options.radius), he[0], he[1], he[2]}));
            const int roundSegments = radius > 0.0
                ? std::max(1, (std::max(options.radiusSegments, 0) + 1) / 2) : 0;
            const double inner[3] = {he[0] - radius, he[1] - radius, he[2] - radius};
            const std::array<AxisSamples, 3> samples = {
                sampleRoundedAxis(he[0], radius, options.widthSegments, roundSegments),
                sampleRoundedAxis(he[1], radius, options.heightSegments, roundSegments),
                sampleRoundedAxis(he[2], radius, options.lengthSegments, roundSegments)
            };

            PrimitiveGeometry geometry;
            uint32_t vertexCounter = 0;
            double p[3] = {};
            double c[3] = {};
            double n[3] = {};
            for (size_t side = 0; side < faces.size(); ++side) {
                const Face& face = faces[side];
                const AxisSamples& uSamples = samples[static_cast<size_t>(face.u)];
                const AxisSamples& vSamples = samples[static_cast<size_t>(face.v)];
                const int uSegments = static_cast<int>(uSamples.coords.size()) - 1;
                const int vSegments = static_cast<int>(vSamples.coords.size()) - 1;

                p[face.n] = face.ns * he[face.n];
                for (int i = 0; i <= uSegments; ++i) {
                    for (int j = 0; j <= vSegments; ++j) {
                        p[face.u] = face.us * uSamples.coords[static_cast<size_t>(i)];
                        p[face.v] = face.vs * vSamples.coords[static_cast<size_t>(j)];

                        // Onto the rounded box: clamp to the inner box, then push back out by the
                        // radius in the direction the point was clamped from.
                        if (radius > 0.0) {
                            double lengthSq = 0.0;
                            for (int k = 0; k < 3; ++k) {
                                c[k] = std::max(-inner[k], std::min(inner[k], p[k]));
                                n[k] = p[k] - c[k];
                                lengthSq += n[k] * n[k];
                            }
                            const double invLength = 1.0 / std::sqrt(lengthSq);
                            for (int k = 0; k < 3; ++k) {
                                n[k] *= invLength;
                                c[k] += n[k] * radius;
                            }
                        } else {
                            for (int k = 0; k < 3; ++k) {
                                c[k] = p[k];
                                n[k] = 0.0;
                            }
                            n[face.n] = face.ns;
                        }

                        const auto u = static_cast<float>(uSamples.fractions[static_cast<size_t>(i)]);
                        const auto v = static_cast<float>(vSamples.fractions[static_cast<size_t>(j)]);
                        pushVertex(geometry,
                            static_cast<float>(c[0]), static_cast<float>(c[1] + options.yOffset), static_cast<float>(c[2]),
                            static_cast<float>(n[0]), static_cast<float>(n[1]), static_cast<float>(n[2]),
                            u, 1.0f - v);
                        // the box's 3x2 cells
                        pushUv1(geometry, u, v, 1.0f / 3.0f, 1.0f / 3.0f,
                            static_cast<float>(side % 3) / 3.0f, static_cast<float>(side / 3) / 3.0f);

                        if (i < uSegments && j < vSegments) {
                            const auto row = static_cast<uint32_t>(vSegments + 1);
                            geometry.indices.insert(geometry.indices.end(), {
                                vertexCounter + row, vertexCounter + 1u, vertexCounter,
                                vertexCounter + row, vertexCounter + row + 1u, vertexCounter + 1u});
                        }
                        vertexCounter++;
                    }
                }
            }

            return withTangents(std::move(geometry));
        }

        PrimitiveGeometry createSphereGeometry()
        {
            return createSphereGeometry(0.5f, 48, 48);
        }

        PrimitiveGeometry createSphereGeometry(const float radius, const int latitudeBandsIn,
            const int longitudeBandsIn)
        {
            PrimitiveGeometry geometry;

            const int latitudeBands = std::max(latitudeBandsIn, 2);
            const int longitudeBands = std::max(longitudeBandsIn, 3);

            for (int lat = 0; lat <= latitudeBands; ++lat) {
                const float theta = static_cast<float>(lat) * PI_F / static_cast<float>(latitudeBands);
                const float sinTheta = std::sin(theta);
                const float cosTheta = std::cos(theta);

                // Each pole vertex is used by a single triangle, so its u is centred on that
                // triangle's segment: the top triangles use the vertex at the
                // segment's end, the bottom ones the vertex at its start.
                float poleUOffset = 0.0f;
                if (lat == 0) {
                    poleUOffset = 0.5f / static_cast<float>(longitudeBands);
                } else if (lat == latitudeBands) {
                    poleUOffset = -0.5f / static_cast<float>(longitudeBands);
                }

                for (int lon = 0; lon <= longitudeBands; ++lon) {
                    const float phi = static_cast<float>(lon) * 2.0f * PI_F / static_cast<float>(longitudeBands) - PI_F * 0.5f;
                    const float sinPhi = std::sin(phi);
                    const float cosPhi = std::cos(phi);

                    const float x = cosPhi * sinTheta;
                    const float y = cosTheta;
                    const float z = sinPhi * sinTheta;
                    const float u = 1.0f - static_cast<float>(lon) / static_cast<float>(longitudeBands) + poleUOffset;
                    const float v = static_cast<float>(lat) / static_cast<float>(latitudeBands);

                    pushVertex(geometry, x * radius, y * radius, z * radius, x, y, z, u, v);
                }
            }

            for (int lat = 0; lat < latitudeBands; ++lat) {
                for (int lon = 0; lon < longitudeBands; ++lon) {
                    const uint32_t first = static_cast<uint32_t>(lat * (longitudeBands + 1) + lon);
                    const uint32_t second = first + static_cast<uint32_t>(longitudeBands + 1);
                    // The triangle that collapses to a line at each pole is left out.
                    if (lat != 0) {
                        geometry.indices.insert(geometry.indices.end(), {first + 1u, second, first});
                    }
                    if (lat != latitudeBands - 1) {
                        geometry.indices.insert(geometry.indices.end(), {first + 1u, second + 1u, second});
                    }
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
                    // A row with a zero radius collapses to the tip, and each of its vertices is
                    // used by a single triangle, so the vertex is centred on that triangle's
                    // segment to keep its normal and u unskewed. The triangles
                    // use the vertex at the segment's start at the top, and its end at the bottom.
                    float tipOffset = 0.0f;
                    if (i == heightSegments && peakRadius == 0.0f) {
                        tipOffset = 0.5f;
                    } else if (i == 0 && baseRadius == 0.0f) {
                        tipOffset = -0.5f;
                    }
                    for (int j = 0; j <= capSegments; ++j) {
                        const float theta = ((static_cast<float>(j) + tipOffset) / static_cast<float>(capSegments)) *
                            2.0f * PI_F - PI_F;
                        const float sinTheta = std::sin(theta);
                        const float cosTheta = std::cos(theta);

                        const Vector3 bottom(sinTheta * baseRadius, -height * 0.5f, cosTheta * baseRadius);
                        const Vector3 top(sinTheta * peakRadius, height * 0.5f, cosTheta * peakRadius);
                        const float t = static_cast<float>(i) / static_cast<float>(heightSegments);
                        const Vector3 pos = bottom + (top - bottom) * t;
                        const Vector3 bottomToTop = (top - bottom).normalized();
                        const Vector3 tangent(cosTheta, 0.0f, -sinTheta);
                        const Vector3 norm = tangent.cross(bottomToTop).normalized();

                        const float u = (static_cast<float>(j) + tipOffset) / static_cast<float>(capSegments);
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
                            // The triangle that collapses to a line at a tip is left out.
                            if (i > 0 || baseRadius > 0.0f) {
                                geometry.indices.insert(geometry.indices.end(), {first, second, third});
                            }
                            if (i < heightSegments - 1 || peakRadius > 0.0f) {
                                geometry.indices.insert(geometry.indices.end(), {second, fourth, third});
                            }
                        }
                    }
                }
            }

            if (roundedCaps) {
                const int latitudeBands = std::max(1, capSegments / 2);
                const int longitudeBands = capSegments;
                const float capOffset = height * 0.5f;
                // Each pole vertex is used by a single triangle, so its u is centred on that
                // triangle's segment: the top cap's triangles use the vertex at the segment's end,
                // the bottom cap's the vertex at its start.
                const float poleUOffset = 0.5f / static_cast<float>(longitudeBands);
                // The caps index from the vertices made so far: with a zero height there is no body
                // (a capsule as tall as it is wide indexed vertices that did not exist).
                const auto topOffset = static_cast<uint32_t>(geometry.positions.size() / 3);

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
                        const float u = 1.0f - static_cast<float>(lon) / static_cast<float>(longitudeBands) +
                            (lat == 0 ? poleUOffset : 0.0f);
                        const float v = static_cast<float>(lat) / static_cast<float>(latitudeBands);
                        pushVertex(geometry,
                            x * peakRadius, y * peakRadius + capOffset, z * peakRadius,
                            x, y, z,
                            u, v);
                        // Top cap in the second third.
                        pushUv1(geometry, u, 1.0f - v, 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f, 0.0f);
                    }
                }

                for (int lat = 0; lat < latitudeBands; ++lat) {
                    for (int lon = 0; lon < longitudeBands; ++lon) {
                        const uint32_t first = static_cast<uint32_t>(lat * (longitudeBands + 1) + lon);
                        const uint32_t second = first + static_cast<uint32_t>(longitudeBands + 1);
                        // The triangle that collapses to a line at the pole is left out.
                        if (lat != 0) {
                            geometry.indices.insert(geometry.indices.end(),
                                {topOffset + first + 1u, topOffset + second, topOffset + first});
                        }
                        geometry.indices.insert(geometry.indices.end(),
                            {topOffset + first + 1u, topOffset + second + 1u, topOffset + second});
                    }
                }

                const auto bottomOffset = static_cast<uint32_t>(geometry.positions.size() / 3);

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
                        const float u = 1.0f - static_cast<float>(lon) / static_cast<float>(longitudeBands) -
                            (lat == latitudeBands ? poleUOffset : 0.0f);
                        const float v = static_cast<float>(lat) / static_cast<float>(latitudeBands);
                        pushVertex(geometry,
                            x * peakRadius, y * peakRadius - capOffset, z * peakRadius,
                            x, y, z,
                            u, v);
                        // Bottom cap in the third third.
                        pushUv1(geometry, u, 1.0f - v, 1.0f / 3.0f, 1.0f / 3.0f, 2.0f / 3.0f, 0.0f);
                    }
                }

                for (int lat = 0; lat < latitudeBands; ++lat) {
                    for (int lon = 0; lon < longitudeBands; ++lon) {
                        const uint32_t first = static_cast<uint32_t>(lat * (longitudeBands + 1) + lon);
                        const uint32_t second = first + static_cast<uint32_t>(longitudeBands + 1);
                        geometry.indices.insert(geometry.indices.end(),
                            {bottomOffset + first + 1u, bottomOffset + second, bottomOffset + first});
                        // The triangle that collapses to a line at the pole is left out.
                        if (lat != latitudeBands - 1) {
                            geometry.indices.insert(geometry.indices.end(),
                                {bottomOffset + first + 1u, bottomOffset + second + 1u, bottomOffset + second});
                        }
                    }
                }
            } else {
                // The caps index from the vertices made so far: the body is skipped at a zero
                // height, and a cap at a zero radius (an inverted cone indexed its
                // top cap past the vertices).
                auto offset = static_cast<uint32_t>(geometry.positions.size() / 3);
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

                offset = static_cast<uint32_t>(geometry.positions.size() / 3);
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
            // Defaults: radius=0.5, height=2, heightSegments=1, sides=20.
            return createConeBaseGeometry(0.5f, 0.5f, 1.0f, 1, 20, true);
        }

        PrimitiveGeometry createPlaneGeometry()
        {
            // createPlane: unit quad on XZ plane, Y up, centered at origin
            PrimitiveGeometry geometry;

            // 4 vertices: (-0.5, 0, -0.5) to (0.5, 0, 0.5)
            // Normal pointing up (+Y). UVs: u = 0..1 along +X, v = 0 at z = -0.5 and 1 at
            // z = +0.5 (`1 - v` of a v running from +Z to -Z). The far edge samples the image's top row, which stands the
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

        PrimitiveGeometry createTorusGeometry(const float tubeRadius, const float ringRadius,
            const float sectorAngleDegrees, const int segments, const int sides)
        {
            // `segments` cross-sections around the ring, `sides` divisions around the
            // tube, lying in the XZ plane. Its UVs are (i / sides, j / segments) after
            // `1 - v`, and UV1 is UV0.
            PrimitiveGeometry geometry;
            const float sectorAngle = sectorAngleDegrees * PI_F / 180.0f;
            for (int i = 0; i <= sides; ++i) {
                const float tube = 2.0f * PI_F * static_cast<float>(i) / static_cast<float>(sides);
                for (int j = 0; j <= segments; ++j) {
                    const float ring = sectorAngle * static_cast<float>(j) / static_cast<float>(segments);
                    const float x = std::cos(ring) * (ringRadius + tubeRadius * std::cos(tube));
                    const float y = std::sin(tube) * tubeRadius;
                    const float z = std::sin(ring) * (ringRadius + tubeRadius * std::cos(tube));
                    const float nx = std::cos(ring) * std::cos(tube);
                    const float ny = std::sin(tube);
                    const float nz = std::sin(ring) * std::cos(tube);
                    pushVertex(geometry, x, y, z, nx, ny, nz,
                        static_cast<float>(i) / static_cast<float>(sides),
                        static_cast<float>(j) / static_cast<float>(segments));

                    if (i < sides && j < segments) {
                        const auto first = static_cast<uint32_t>(i * (segments + 1) + j);
                        const auto second = static_cast<uint32_t>((i + 1) * (segments + 1) + j);
                        const auto third = static_cast<uint32_t>(i * (segments + 1) + j + 1);
                        const auto fourth = static_cast<uint32_t>((i + 1) * (segments + 1) + j + 1);
                        geometry.indices.insert(geometry.indices.end(), {first, second, third});
                        geometry.indices.insert(geometry.indices.end(), {second, fourth, third});
                    }
                }
            }
            return withTangents(std::move(geometry));
        }

        PrimitiveGeometry createTorusGeometry()
        {
            // Defaults: tube 0.2, ring 0.3, a full ring, 30 x 20.
            return createTorusGeometry(0.2f, 0.3f, 360.0f, 30, 20);
        }

        PrimitiveGeometry createCircleGeometry(const float radius, const int sectors, const int rings,
            const float ringExponent)
        {
            // A flat disc in the XZ plane facing +Y: a centre vertex and `rings` concentric rings
            // of `sectors` vertices each. Ring r sits at radius * (r / rings)^ringExponent, so an
            // exponent above 1 gathers the rings toward the centre. The UVs map the bounding
            // square planarly, v = 0 at -Z as on the plane, and UV1 is UV0.
            PrimitiveGeometry geometry;
            const int sectorCount = std::max(sectors, 3);
            const int ringCount = std::max(rings, 1);
            const auto uvOf = [radius](const float x, const float z) {
                return std::array<float, 2>{0.5f + x / (2.0f * radius), 0.5f + z / (2.0f * radius)};
            };
            pushVertex(geometry, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 0.5f);
            for (int r = 1; r <= ringCount; ++r) {
                const float ringRadius = radius *
                    static_cast<float>(std::pow(static_cast<double>(r) / ringCount, static_cast<double>(ringExponent)));
                for (int j = 0; j < sectorCount; ++j) {
                    const double angle = 2.0 * std::numbers::pi * j / sectorCount;
                    const float x = ringRadius * static_cast<float>(std::cos(angle));
                    const float z = ringRadius * static_cast<float>(std::sin(angle));
                    const auto uv = uvOf(x, z);
                    pushVertex(geometry, x, 0.0f, z, 0.0f, 1.0f, 0.0f, uv[0], uv[1]);
                }
            }

            // The j-th vertex of ring r, wrapping around the seam.
            const auto ringVertex = [sectorCount](const int r, const int j) {
                return static_cast<uint32_t>(1 + (r - 1) * sectorCount + (j % sectorCount));
            };
            // A fan from the centre to the innermost ring, then a band of quads between rings.
            for (int j = 0; j < sectorCount; ++j) {
                geometry.indices.insert(geometry.indices.end(), {0u, ringVertex(1, j + 1), ringVertex(1, j)});
            }
            for (int r = 1; r < ringCount; ++r) {
                for (int j = 0; j < sectorCount; ++j) {
                    const uint32_t inner = ringVertex(r, j);
                    const uint32_t innerNext = ringVertex(r, j + 1);
                    const uint32_t outer = ringVertex(r + 1, j);
                    const uint32_t outerNext = ringVertex(r + 1, j + 1);
                    geometry.indices.insert(geometry.indices.end(), {inner, outerNext, outer, inner, innerNext, outerNext});
                }
            }
            return withTangents(std::move(geometry));
        }
}
