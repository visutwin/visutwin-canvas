// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 25.09.2026
//
// OBJ and STL files written to disk, parsed, and the vertex and index buffers read
// back: positions, normals, UVs, winding and the config transforms.
//
// The rule every case checks is the one a render cannot tell apart from shading: a
// vertex normal must point to the side the triangle's WINDING says is its front, or
// the surface is lit inside out under back-face culling. Checked per emitted
// triangle, so a normal belonging to another face also fails.
//
// CPU only: a stub device keeps the buffers' bytes.
//
// Also the parsers' one shared generateTangents (packedVertex.h): the tangent follows +u, is
// unit and perpendicular to the normal, the handedness puts the bitangent toward +v, and a
// vertex with degenerate UVs still gets a unit tangent perpendicular to its normal.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/parsers/glbContainerResource.h"
#include "framework/parsers/objParser.h"
#include "framework/parsers/packedVertex.h"
#include "framework/parsers/phongMaterial.h"
#include "framework/parsers/stlParser.h"
#include "platform/graphics/indexBuffer.h"
#include "platform/graphics/vertexBuffer.h"
#include "scene/mesh.h"
#include "support/check.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    struct Geometry
    {
        std::vector<PackedVertex> vertices;
        std::vector<uint32_t> indices;
        BoundingBox aabb;
    };

    Geometry read(const GlbMeshPayload& payload)
    {
        Geometry g;
        const auto vb = payload.mesh->getVertexBuffer();
        const auto ib = payload.mesh->getIndexBuffer();
        if (!vb || !ib || vb->format()->size() != sizeof(PackedVertex)) {
            return g;
        }
        g.vertices.resize(vb->storage().size() / sizeof(PackedVertex));
        std::memcpy(g.vertices.data(), vb->storage().data(), g.vertices.size() * sizeof(PackedVertex));
        const auto& bytes = ib->storage();
        if (ib->format() == INDEXFORMAT_UINT16) {
            for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
                uint16_t v;
                std::memcpy(&v, bytes.data() + i, 2);
                g.indices.push_back(v);
            }
        } else {
            for (size_t i = 0; i + 3 < bytes.size(); i += 4) {
                uint32_t v;
                std::memcpy(&v, bytes.data() + i, 4);
                g.indices.push_back(v);
            }
        }
        g.aabb = payload.mesh->aabb();
        return g;
    }

    Geometry only(const std::unique_ptr<GlbContainerResource>& container)
    {
        if (!container || container->meshPayloads().size() != 1) {
            return {};
        }
        return read(container->meshPayloads()[0]);
    }

    Vector3 position(const PackedVertex& v) { return Vector3(v.px, v.py, v.pz); }
    Vector3 normal(const PackedVertex& v) { return Vector3(v.nx, v.ny, v.nz); }

    Vector3 windingNormal(const Geometry& g, const size_t tri)
    {
        const Vector3 a = position(g.vertices[g.indices[tri * 3 + 0]]);
        const Vector3 b = position(g.vertices[g.indices[tri * 3 + 1]]);
        const Vector3 c = position(g.vertices[g.indices[tri * 3 + 2]]);
        return (b - a).cross(c - a).normalized();
    }

    /// Every corner's normal on the winding's front side; `exact` also demands it BE the
    /// face normal (flat shading).
    bool normalsFollowWinding(const Geometry& g, const bool exact)
    {
        if (g.indices.empty() || g.indices.size() % 3 != 0) {
            return false;
        }
        for (size_t t = 0; t < g.indices.size() / 3; ++t) {
            const Vector3 face = windingNormal(g, t);
            for (int c = 0; c < 3; ++c) {
                const Vector3 n = normal(g.vertices[g.indices[t * 3 + c]]);
                const float d = n.dot(face);
                if (exact ? d < 0.9999f : d <= 0.0f) {
                    return false;
                }
            }
        }
        return true;
    }

    /// The length of the difference within eps (not per component).
    bool withinDistance(const Vector3& a, const Vector3& b, const float eps = 1e-5f)
    {
        return (a - b).length() <= eps;
    }

    bool hasVertexAt(const Geometry& g, const Vector3& p, const float eps = 1e-5f)
    {
        for (const auto& v : g.vertices) {
            if (withinDistance(position(v), p, eps)) {
                return true;
            }
        }
        return false;
    }

    std::filesystem::path dir;

    std::string write(const std::string& name, const std::string& text)
    {
        const auto path = dir / name;
        std::ofstream(path, std::ios::binary) << text;
        return path.string();
    }

    // A unit cube, every face wound counter-clockwise seen from OUTSIDE, as quads.
    const char* kCubeVertices =
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n";
    const char* kCubeFaces =
        "f 1 4 3 2\n"   // -Z
        "f 5 6 7 8\n"   // +Z
        "f 1 2 6 5\n"   // -Y
        "f 4 8 7 3\n"   // +Y
        "f 1 5 8 4\n"   // -X
        "f 2 3 7 6\n";  // +X

    // The same cube's twelve triangles, for STL.
    struct Tri { float v[9]; };
    std::vector<Tri> cubeTriangles()
    {
        const float p[8][3] = {{0,0,0},{1,0,0},{1,1,0},{0,1,0},{0,0,1},{1,0,1},{1,1,1},{0,1,1}};
        const int quads[6][4] = {{0,3,2,1},{4,5,6,7},{0,1,5,4},{3,7,6,2},{0,4,7,3},{1,2,6,5}};
        std::vector<Tri> tris;
        for (const auto& q : quads) {
            const int a[3] = {q[0], q[1], q[2]};
            const int b[3] = {q[0], q[2], q[3]};
            for (const int* corner : {a, b}) {
                Tri tri{};
                for (int c = 0; c < 3; ++c) {
                    for (int k = 0; k < 3; ++k) {
                        tri.v[c * 3 + k] = p[corner[c]][k];
                    }
                }
                tris.push_back(tri);
            }
        }
        return tris;
    }

    std::string asciiStl(const std::vector<Tri>& tris)
    {
        std::string s = "solid cube\n";
        for (const auto& t : tris) {
            // A deliberately WRONG stored normal: the parser must derive it from the vertices.
            s += "  facet normal 0 0 0\n    outer loop\n";
            for (int c = 0; c < 3; ++c) {
                s += "      vertex " + std::to_string(t.v[c * 3]) + " " + std::to_string(t.v[c * 3 + 1]) + " " +
                     std::to_string(t.v[c * 3 + 2]) + "\n";
            }
            s += "    endloop\n  endfacet\n";
        }
        return s + "endsolid cube\n";
    }

    std::string binaryStl(const std::vector<Tri>& tris, const std::string& header)
    {
        std::string s(80, '\0');
        std::memcpy(s.data(), header.data(), std::min<size_t>(header.size(), 80));
        const uint32_t count = static_cast<uint32_t>(tris.size());
        s.append(reinterpret_cast<const char*>(&count), 4);
        for (const auto& t : tris) {
            const float n[3] = {0.0f, 0.0f, 0.0f};
            s.append(reinterpret_cast<const char*>(n), 12);
            s.append(reinterpret_cast<const char*>(t.v), 36);
            s.append(2, '\0');
        }
        return s;
    }

    Vector3 centre(0.5f, 0.5f, 0.5f);

    /// Normals point AWAY from the cube's centre, i.e. outward.
    bool outward(const Geometry& g, const Vector3& c = centre)
    {
        for (size_t t = 0; t < g.indices.size() / 3; ++t) {
            const Vector3 a = position(g.vertices[g.indices[t * 3]]);
            const Vector3 b = position(g.vertices[g.indices[t * 3 + 1]]);
            const Vector3 d = position(g.vertices[g.indices[t * 3 + 2]]);
            const Vector3 mid = (a + b + d) * (1.0f / 3.0f);
            if (windingNormal(g, t).dot(mid - c) <= 0.0f) {
                return false;
            }
        }
        return true;
    }
}

int main()
{
    std::cout << std::unitbuf;
    auto device = std::make_shared<StubGraphicsDevice>(
        StubGraphicsDevice::Options{.cpuBuffers = true, .keepIndexData = true});
    dir = std::filesystem::temp_directory_path() / "visutwin-obj-stl-tests";
    std::filesystem::create_directories(dir);

    std::cout << "Phong shininess to roughness (shared by the OBJ and Assimp parsers)\n";
    {
        // alpha = sqrt(2 / (n + 2)) and roughness = sqrt(alpha): n = 0 fully rough, n = 30 at
        // alpha 0.25 (roughness 0.5), and higher exponents always smoother.
        check(roughnessFromShininess(0.0f) == 1.0f, "an exponent of 0 is fully rough");
        check(std::abs(roughnessFromShininess(30.0f) - 0.5f) < 1e-6f, "an exponent of 30 is roughness 0.5");
        bool decreasing = true;
        for (float n = 1.0f; n < 5000.0f; n *= 2.0f) {
            decreasing = decreasing && roughnessFromShininess(2.0f * n) < roughnessFromShininess(n);
        }
        check(decreasing, "a higher exponent is always smoother");
        check(roughnessFromShininess(-5.0f) == 1.0f, "a negative exponent clamps to fully rough");
    }

    std::cout << "OBJ: a textured quad with normals\n";
    {
        const auto path = write("quad.obj",
            "v 0 0 0\nv 2 0 0\nv 2 1 0\nv 0 1 0\n"
            "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
            "vn 0 0 1\n"
            "f 1/1/1 2/2/1 3/3/1 4/4/1\n");
        const auto g = only(ObjParser::parse(path, device));
        check(g.vertices.size() == 4 && g.indices.size() == 6, "one quad: 4 shared vertices, 2 triangles (" +
            std::to_string(g.vertices.size()) + ", " + std::to_string(g.indices.size()) + ")");
        check(!g.indices.empty() && normalsFollowWinding(g, true), "the file's normal agrees with the winding");
        bool uv = !g.vertices.empty();
        for (const auto& v : g.vertices) {
            // OBJ's v runs up from the BOTTOM; here v = 0 is the image's TOP row.
            uv = uv && std::abs(v.u - v.px / 2.0f) < 1e-6f && std::abs(v.v - (1.0f - v.py)) < 1e-6f &&
                 v.u1 == v.u && v.v1 == v.v;
        }
        check(uv, "UVs flipped to top-left origin (v = 1 - vt), UV1 = UV0");
        check(withinDistance(g.aabb.center(), Vector3(1.0f, 0.5f, 0.0f)) &&
              withinDistance(g.aabb.halfExtents(), Vector3(1.0f, 0.5f, 0.0f)),
            "the bounds are the quad's");
    }

    std::cout << "\nOBJ: a cube with NO normals, flat ('s off')\n";
    {
        const auto path = write("cube-flat.obj", std::string(kCubeVertices) + "s off\n" + kCubeFaces);
        const auto g = only(ObjParser::parse(path, device));
        check(g.indices.size() == 36, "12 triangles");
        check(outward(g), "wound outward");
        check(normalsFollowWinding(g, true), "every corner carries ITS face's normal, not a neighbour's");
        check(g.vertices.size() == 24, "so each corner is split per face: 24 vertices (" +
            std::to_string(g.vertices.size()) + ")");
    }

    std::cout << "\nOBJ: the same cube smoothed ('s 1')\n";
    {
        const auto path = write("cube-smooth.obj", std::string(kCubeVertices) + "s 1\n" + kCubeFaces);
        const auto g = only(ObjParser::parse(path, device));
        check(g.vertices.size() == 8, "one vertex per corner");
        bool diagonal = g.vertices.size() == 8;
        for (const auto& v : g.vertices) {
            diagonal = diagonal && withinDistance(normal(v), (position(v) - centre).normalized(), 1e-5f);
        }
        check(diagonal, "each normal is the corner's diagonal, pointing out");
        check(normalsFollowWinding(g, false), "and on every triangle's front side");
    }

    std::cout << "\nOBJ: config transforms\n";
    {
        const auto path = write("cube-scaled.obj", std::string(kCubeVertices) + "s off\n" + kCubeFaces);
        ObjParserConfig config;
        config.uniformScale = 2.0f;
        config.flipYZ = true;
        const auto g = only(ObjParser::parse(path, device, config));
        check(hasVertexAt(g, Vector3(2.0f, 0.0f, 0.0f)) && hasVertexAt(g, Vector3(0.0f, 2.0f, 0.0f)) &&
              hasVertexAt(g, Vector3(0.0f, 0.0f, -2.0f)),
            "scale 2 then (x, y, z) -> (x, z, -y): (1,0,0) (0,0,1) (0,1,0) land at (2,0,0) (0,2,0) (0,0,-2)");
        check(outward(g, Vector3(1.0f, 1.0f, -1.0f)) && normalsFollowWinding(g, true),
            "a rotation keeps the winding outward and the normals with it");

        ObjParserConfig flip;
        flip.flipWinding = true;
        const auto f = only(ObjParser::parse(path, device, flip));
        check(!f.indices.empty() && !outward(f), "flipWinding reverses every triangle");
        check(normalsFollowWinding(f, true),
            "and normals the parser DERIVED from the winding follow it (a file's own normals are the file's)");
    }

    std::cout << "\nOBJ: two objects\n";
    {
        const auto path = write("two.obj",
            "o left\nv -3 0 0\nv -2 0 0\nv -2 1 0\nf 1 2 3\n"
            "o right\nv 2 0 0\nv 3 0 0\nv 3 1 0\nv 2 1 0\nf 4 5 6 7\n");
        const auto container = ObjParser::parse(path, device);
        const bool shape = container && container->nodePayloads().size() == 2 &&
                           container->meshPayloads().size() == 2;
        check(shape, "two nodes, two payloads");
        if (shape) {
            const auto& nodes = container->nodePayloads();
            const auto left = read(container->meshPayloads()[nodes[0].meshPayloadIndices.at(0)]);
            const auto right = read(container->meshPayloads()[nodes[1].meshPayloadIndices.at(0)]);
            check(nodes[0].name == "left" && left.indices.size() == 3 && left.aabb.center().getX() < 0.0f,
                "'left' holds the triangle");
            check(nodes[1].name == "right" && right.indices.size() == 6 && right.aabb.center().getX() > 0.0f,
                "'right' holds the quad");
        }
    }

    const auto tris = cubeTriangles();
    std::cout << "\nSTL: ASCII and binary, flat\n";
    Geometry ascii;
    {
        ascii = only(StlParser::parse(write("cube-ascii.stl", asciiStl(tris)), device));
        check(ascii.vertices.size() == 36 && ascii.indices.size() == 36, "ASCII: 12 facets, 36 flat vertices");
        check(outward(ascii) && normalsFollowWinding(ascii, true),
            "normals come from the vertices (the file's are zero) and agree with the winding");
        const auto binary = only(StlParser::parse(write("cube-binary.stl", binaryStl(tris, "exported")), device));
        bool same = binary.vertices.size() == ascii.vertices.size() && binary.indices == ascii.indices;
        for (size_t i = 0; same && i < binary.vertices.size(); ++i) {
            same = std::memcmp(&binary.vertices[i], &ascii.vertices[i], sizeof(PackedVertex)) == 0;
        }
        check(same, "binary gives the same buffers, byte for byte");
        // Many exporters begin a BINARY header with "solid", which is how ASCII files begin.
        const auto solid = only(StlParser::parse(write("cube-solid.stl", binaryStl(tris, "solid exported")), device));
        check(solid.indices.size() == 36, "a binary file whose header starts \"solid\" is still read as binary");
        check(withinDistance(ascii.aabb.center(), centre) &&
              withinDistance(ascii.aabb.halfExtents(), centre), "the bounds are the cube's");
    }

    std::cout << "\nSTL: smooth normals and crease angle\n";
    {
        StlParserConfig config;
        config.generateSmoothNormals = true;
        config.creaseAngle = 40.0f;   // below the cube's 90-degree edges: stays faceted
        const auto creased = only(StlParser::parse(write("cube-c.stl", binaryStl(tris, "c")), device, config));
        check(creased.vertices.size() == 24 && normalsFollowWinding(creased, true),
            "edges sharper than the crease stay hard: 24 vertices, face normals (" +
            std::to_string(creased.vertices.size()) + ")");
        config.creaseAngle = 100.0f;
        const auto smooth = only(StlParser::parse(write("cube-s.stl", binaryStl(tris, "s")), device, config));
        bool diagonal = smooth.vertices.size() == 8;
        for (const auto& v : smooth.vertices) {
            diagonal = diagonal && withinDistance(normal(v), (position(v) - centre).normalized(), 1e-5f);
        }
        check(smooth.indices.size() == 36 && diagonal && normalsFollowWinding(smooth, false),
            "above it they are smoothed: 8 vertices, each normal the corner's diagonal whichever "
            "way the faces were split");
    }

    std::cout << "\nSTL: config transforms\n";
    {
        StlParserConfig config;
        config.uniformScale = 0.001f;   // mm -> m
        config.flipYZ = true;
        const auto g = only(StlParser::parse(write("cube-t.stl", binaryStl(tris, "t")), device, config));
        check(hasVertexAt(g, Vector3(0.0f, 0.0f, -0.001f), 1e-8f) && hasVertexAt(g, Vector3(0.0f, 0.001f, 0.0f), 1e-8f),
            "scale and (x, y, z) -> (x, z, -y)");
        check(normalsFollowWinding(g, true) && outward(g, Vector3(0.0005f, 0.0005f, -0.0005f)),
            "still outward with matching normals");
        for (const bool smooth : {false, true}) {
            StlParserConfig flip;
            flip.flipWinding = true;
            flip.generateSmoothNormals = smooth;
            flip.creaseAngle = 40.0f;
            const auto f = only(StlParser::parse(write("cube-f.stl", binaryStl(tris, "f")), device, flip));
            check(!f.indices.empty() && !outward(f) && normalsFollowWinding(f, true),
                std::string("flipWinding (") + (smooth ? "smooth" : "flat") +
                ") reverses the triangles AND the normals derived from them");
        }
    }

    {
        // A quad in the XY plane facing +Z, u along +X and v along +Y, then the same quad with
        // v running down (-Y): only the handedness may change between the two.
        const auto quad = [](const float vSign) {
            std::vector<PackedVertex> v(4);
            const float corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            for (int i = 0; i < 4; ++i) {
                v[i] = PackedVertex{corners[i][0], corners[i][1], 0.0f, 0.0f, 0.0f, 1.0f,
                                    corners[i][0], vSign > 0 ? corners[i][1] : 1.0f - corners[i][1],
                                    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            }
            return v;
        };
        const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
        auto up = quad(1.0f);
        generateTangents(up, indices);
        auto down = quad(-1.0f);
        generateTangents(down, indices);
        bool along = true;
        for (size_t i = 0; i < 4; ++i) {
            along &= std::abs(up[i].tx - 1.0f) < 1e-6f && std::abs(up[i].ty) < 1e-6f && std::abs(up[i].tz) < 1e-6f;
            along &= up[i].tw == 1.0f && down[i].tw == -1.0f;
        }
        check(along, "generateTangents: tangent along +u, bitangent toward +v (w +1), flipped v gives w -1");

        // Degenerate UVs (all the same): the fallback is unit and perpendicular to the normal.
        std::vector<PackedVertex> flat(3);
        const Vector3 n = Vector3(0.3f, 0.8f, 0.52f).normalized();
        for (int i = 0; i < 3; ++i) {
            flat[i] = PackedVertex{static_cast<float>(i), static_cast<float>(i * i), 0.0f,
                                   n.getX(), n.getY(), n.getZ(), 0.5f, 0.5f, 0, 0, 0, 0, 0, 0};
        }
        generateTangents(flat, {});
        const Vector3 t(flat[0].tx, flat[0].ty, flat[0].tz);
        check(std::abs(t.length() - 1.0f) < 1e-5f && std::abs(t.dot(n)) < 1e-5f,
              "generateTangents: degenerate UVs give a unit tangent perpendicular to the normal");

        float tx, ty, tz, tw;
        tangentFromNormal(n.getX(), n.getY(), n.getZ(), tx, ty, tz, tw);
        const Vector3 fallback(tx, ty, tz);
        check(std::abs(fallback.length() - 1.0f) < 1e-5f && std::abs(fallback.dot(n)) < 1e-5f && tw == 1.0f,
              "tangentFromNormal: unit, perpendicular to the normal, handedness 1");
    }

    std::filesystem::remove_all(dir);
    return finish("OBJ/STL round-trip");
}
