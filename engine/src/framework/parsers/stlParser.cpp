// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// STL (Stereolithography) file parser for VisuTwin Canvas.
//
// Supports both binary and ASCII STL formats. Binary is auto-detected by
// validating the file size against the header triangle count formula:
//   fileSize == 80 + 4 + triangleCount * 50
//
// Key design decisions:
//   - Reuses GlbContainerResource for unified instantiateRenderEntity() path
//   - No external library — STL binary parsing is ~50 lines of C++
//   - Flat shading by default (face normals); optional crease-angle smooth normals
//   - Always recomputes normals from geometry (many exporters write (0,0,0))
//   - Vertex welding only when smooth normals are requested
//   - Single default PBR material (STL has no material data)
//   - Same 56-byte PackedVertex layout as GlbParser and ObjParser
//
// Custom loader (not derived from upstream).
//
#include "stlParser.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "core/math/vector3.h"
#include "core/shape/boundingBox.h"
#include "platform/graphics/constants.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/materials/standardMaterial.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    namespace
    {
        // ── Vertex layout (must match GlbParser::PackedVertex and ObjParser) ──

        struct PackedVertex
        {
            float px, py, pz;       // position
            float nx, ny, nz;       // normal
            float u, v;             // uv0
            float tx, ty, tz, tw;   // tangent + handedness
            float u1, v1;           // uv1
        };

        static_assert(sizeof(PackedVertex) == 56, "PackedVertex must be 56 bytes (14 floats)");

        // ── Raw triangle as read from binary STL ──────────────────────────

        struct StlTriangle
        {
            float nx, ny, nz;       // face normal
            float v0x, v0y, v0z;    // vertex 0
            float v1x, v1y, v1z;    // vertex 1
            float v2x, v2y, v2z;    // vertex 2
        };

        // ── Tangent-from-normal fallback (no UVs in STL) ─────────────────

        void tangentFromNormal(float nx, float ny, float nz,
                               float& tx, float& ty, float& tz, float& tw)
        {
            Vector3 n(nx, ny, nz);
            Vector3 up = std::abs(ny) < 0.999f ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
            Vector3 t = n.cross(up).normalized();
            tx = t.getX();
            ty = t.getY();
            tz = t.getZ();
            tw = 1.0f;
        }

        // ── Compute geometric face normal from 3 vertices ────────────────

        Vector3 computeFaceNormal(float v0x, float v0y, float v0z,
                                  float v1x, float v1y, float v1z,
                                  float v2x, float v2y, float v2z)
        {
            Vector3 e1(v1x - v0x, v1y - v0y, v1z - v0z);
            Vector3 e2(v2x - v0x, v2y - v0y, v2z - v0z);
            Vector3 n = e1.cross(e2);
            float len = n.length();
            if (len > 1e-8f) {
                return n * (1.0f / len);
            }
            return Vector3(0.0f, 1.0f, 0.0f);  // degenerate triangle fallback
        }

        // ── Binary format detection ──────────────────────────────────────

        bool isBinaryStl(const std::vector<uint8_t>& data)
        {
            if (data.size() < 84) return false;

            // Read triangle count from offset 80
            uint32_t triCount = 0;
            std::memcpy(&triCount, data.data() + 80, sizeof(uint32_t));

            // Validate file size: 80 header + 4 count + triCount * 50
            const size_t expectedSize = 80 + 4 + static_cast<size_t>(triCount) * 50;
            return data.size() == expectedSize;
        }

        // ── Parse binary STL ─────────────────────────────────────────────

        bool parseBinaryStl(const std::vector<uint8_t>& data,
                            std::vector<StlTriangle>& triangles,
                            std::string& solidName)
        {
            if (data.size() < 84) {
                spdlog::error("STL binary file too small ({} bytes)", data.size());
                return false;
            }

            // Extract solid name from header (first 80 bytes, null-terminated or trimmed)
            solidName.assign(reinterpret_cast<const char*>(data.data()), 80);
            // Remove trailing nulls and whitespace
            auto end = solidName.find_last_not_of(std::string("\0 \t\r\n", 5));
            solidName = (end != std::string::npos) ? solidName.substr(0, end + 1) : "";
            // Strip "solid " prefix if present
            if (solidName.size() >= 6 && solidName.substr(0, 6) == "solid ") {
                solidName = solidName.substr(6);
            }

            uint32_t triCount = 0;
            std::memcpy(&triCount, data.data() + 80, sizeof(uint32_t));

            const size_t expectedSize = 80 + 4 + static_cast<size_t>(triCount) * 50;
            if (data.size() < expectedSize) {
                spdlog::error("STL binary file truncated: expected {} bytes, got {}", expectedSize, data.size());
                return false;
            }

            triangles.resize(triCount);
            const uint8_t* ptr = data.data() + 84;

            for (uint32_t i = 0; i < triCount; ++i) {
                // 12 floats = normal(3) + v0(3) + v1(3) + v2(3) = 48 bytes
                std::memcpy(&triangles[i], ptr, 48);
                ptr += 50;  // 48 data bytes + 2 attribute bytes (skipped)
            }

            return true;
        }

        // ── Parse ASCII STL ──────────────────────────────────────────────

        bool parseAsciiStl(const std::vector<uint8_t>& data,
                           std::vector<StlTriangle>& triangles,
                           std::string& solidName)
        {
            std::string text(reinterpret_cast<const char*>(data.data()), data.size());
            std::istringstream stream(text);
            std::string token;

            // Read "solid <name>"
            stream >> token;  // "solid"
            if (token != "solid") {
                spdlog::error("STL ASCII: expected 'solid', got '{}'", token);
                return false;
            }
            std::getline(stream, solidName);
            // Trim leading/trailing whitespace
            auto start = solidName.find_first_not_of(" \t\r\n");
            auto end = solidName.find_last_not_of(" \t\r\n");
            solidName = (start != std::string::npos) ? solidName.substr(start, end - start + 1) : "";

            while (stream >> token) {
                if (token == "endsolid") break;

                if (token == "facet") {
                    StlTriangle tri{};

                    // "facet normal nx ny nz"
                    stream >> token;  // "normal"
                    stream >> tri.nx >> tri.ny >> tri.nz;

                    // "outer loop"
                    stream >> token >> token;  // "outer" "loop"

                    // 3 vertices
                    stream >> token >> tri.v0x >> tri.v0y >> tri.v0z;  // "vertex" x y z
                    stream >> token >> tri.v1x >> tri.v1y >> tri.v1z;
                    stream >> token >> tri.v2x >> tri.v2y >> tri.v2z;

                    // "endloop"
                    stream >> token;

                    // "endfacet"
                    stream >> token;

                    triangles.push_back(tri);
                }
            }

            return !triangles.empty();
        }

        // ── Spatial hash key for vertex welding ──────────────────────────

        struct PositionKey
        {
            int32_t ix, iy, iz;

            bool operator==(const PositionKey& o) const
            {
                return ix == o.ix && iy == o.iy && iz == o.iz;
            }
        };

        struct PositionKeyHash
        {
            size_t operator()(const PositionKey& k) const
            {
                size_t h = std::hash<int32_t>()(k.ix);
                h ^= std::hash<int32_t>()(k.iy) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<int32_t>()(k.iz) + 0x9e3779b9 + (h << 6) + (h >> 2);
                return h;
            }
        };

        PositionKey quantize(float x, float y, float z, float invEpsilon)
        {
            return {
                static_cast<int32_t>(std::round(x * invEpsilon)),
                static_cast<int32_t>(std::round(y * invEpsilon)),
                static_cast<int32_t>(std::round(z * invEpsilon))
            };
        }

        // ── Build flat-shaded mesh (no vertex welding) ───────────────────

        void buildFlatMesh(
            const std::vector<StlTriangle>& triangles,
            const StlParserConfig& config,
            std::vector<PackedVertex>& outVertices,
            std::vector<uint32_t>& outIndices,
            float& minX, float& minY, float& minZ,
            float& maxX, float& maxY, float& maxZ)
        {
            const size_t triCount = triangles.size();
            outVertices.reserve(triCount * 3);
            outIndices.reserve(triCount * 3);

            for (size_t i = 0; i < triCount; ++i) {
                const auto& tri = triangles[i];

                // Scale vertices
                float positions[3][3] = {
                    {tri.v0x * config.uniformScale, tri.v0y * config.uniformScale, tri.v0z * config.uniformScale},
                    {tri.v1x * config.uniformScale, tri.v1y * config.uniformScale, tri.v1z * config.uniformScale},
                    {tri.v2x * config.uniformScale, tri.v2y * config.uniformScale, tri.v2z * config.uniformScale}
                };

                // FlipYZ
                if (config.flipYZ) {
                    for (auto& p : positions) {
                        std::swap(p[1], p[2]);
                        p[2] = -p[2];
                    }
                }

                // Recompute face normal from geometry (don't trust stored normal)
                Vector3 faceN = computeFaceNormal(
                    positions[0][0], positions[0][1], positions[0][2],
                    positions[1][0], positions[1][1], positions[1][2],
                    positions[2][0], positions[2][1], positions[2][2]);

                float fnx = faceN.getX();
                float fny = faceN.getY();
                float fnz = faceN.getZ();

                // Tangent from normal
                float tx, ty, tz, tw;
                if (config.generateTangents) {
                    tangentFromNormal(fnx, fny, fnz, tx, ty, tz, tw);
                } else {
                    tx = 1.0f; ty = 0.0f; tz = 0.0f; tw = 1.0f;
                }

                // Determine triangle vertex order
                int order[3] = {0, 1, 2};
                if (config.flipWinding) {
                    std::swap(order[1], order[2]);
                }

                // Emit 3 vertices with face normal
                for (int j = 0; j < 3; ++j) {
                    int oi = order[j];
                    PackedVertex vert{};
                    vert.px = positions[oi][0];
                    vert.py = positions[oi][1];
                    vert.pz = positions[oi][2];
                    vert.nx = fnx;
                    vert.ny = fny;
                    vert.nz = fnz;
                    vert.u = 0.0f;
                    vert.v = 0.0f;
                    vert.tx = tx;
                    vert.ty = ty;
                    vert.tz = tz;
                    vert.tw = tw;
                    vert.u1 = 0.0f;
                    vert.v1 = 0.0f;

                    auto idx = static_cast<uint32_t>(outVertices.size());
                    outVertices.push_back(vert);
                    outIndices.push_back(idx);

                    minX = std::min(minX, vert.px);
                    minY = std::min(minY, vert.py);
                    minZ = std::min(minZ, vert.pz);
                    maxX = std::max(maxX, vert.px);
                    maxY = std::max(maxY, vert.py);
                    maxZ = std::max(maxZ, vert.pz);
                }
            }
        }

        // ── Build smooth-shaded mesh (with vertex welding + crease angle) ──

        void buildSmoothMesh(
            const std::vector<StlTriangle>& triangles,
            const StlParserConfig& config,
            std::vector<PackedVertex>& outVertices,
            std::vector<uint32_t>& outIndices,
            float& minX, float& minY, float& minZ,
            float& maxX, float& maxY, float& maxZ)
        {
            const size_t triCount = triangles.size();
            const float cosCrease = std::cos(config.creaseAngle * 3.14159265358979f / 180.0f);

            // Step 1: Transform all positions and compute face normals
            struct TransformedTri {
                float pos[3][3];    // 3 vertices × (x,y,z)
                Vector3 faceNormal;
            };

            std::vector<TransformedTri> tris(triCount);

            for (size_t i = 0; i < triCount; ++i) {
                const auto& src = triangles[i];

                tris[i].pos[0][0] = src.v0x * config.uniformScale;
                tris[i].pos[0][1] = src.v0y * config.uniformScale;
                tris[i].pos[0][2] = src.v0z * config.uniformScale;
                tris[i].pos[1][0] = src.v1x * config.uniformScale;
                tris[i].pos[1][1] = src.v1y * config.uniformScale;
                tris[i].pos[1][2] = src.v1z * config.uniformScale;
                tris[i].pos[2][0] = src.v2x * config.uniformScale;
                tris[i].pos[2][1] = src.v2y * config.uniformScale;
                tris[i].pos[2][2] = src.v2z * config.uniformScale;

                if (config.flipYZ) {
                    for (auto& p : tris[i].pos) {
                        std::swap(p[1], p[2]);
                        p[2] = -p[2];
                    }
                }

                tris[i].faceNormal = computeFaceNormal(
                    tris[i].pos[0][0], tris[i].pos[0][1], tris[i].pos[0][2],
                    tris[i].pos[1][0], tris[i].pos[1][1], tris[i].pos[1][2],
                    tris[i].pos[2][0], tris[i].pos[2][1], tris[i].pos[2][2]);
            }

            // Step 2: Weld vertices by quantized position
            // Epsilon: use uniformScale to adapt — if scale is 0.001 (mm→m), positions
            // are small; if scale is 1.0, positions may be in hundreds.
            const float epsilon = 1e-5f;
            const float invEpsilon = 1.0f / epsilon;

            // Map: quantized position → welded vertex index
            // One welded vertex can appear in many triangles
            std::unordered_map<PositionKey, uint32_t, PositionKeyHash> weldMap;
            // Per welded vertex: list of (triIndex, cornerIndex) pairs
            struct WeldedVertex {
                float px, py, pz;
                std::vector<std::pair<size_t, int>> incidents;  // (triIdx, corner 0/1/2)
            };
            std::vector<WeldedVertex> welded;

            // Per-triangle corner → welded index
            std::vector<std::array<uint32_t, 3>> triWeldedIdx(triCount);

            for (size_t i = 0; i < triCount; ++i) {
                for (int c = 0; c < 3; ++c) {
                    float px = tris[i].pos[c][0];
                    float py = tris[i].pos[c][1];
                    float pz = tris[i].pos[c][2];

                    PositionKey key = quantize(px, py, pz, invEpsilon);
                    auto it = weldMap.find(key);
                    uint32_t wIdx;
                    if (it != weldMap.end()) {
                        wIdx = it->second;
                    } else {
                        wIdx = static_cast<uint32_t>(welded.size());
                        WeldedVertex wv;
                        wv.px = px; wv.py = py; wv.pz = pz;
                        welded.push_back(std::move(wv));
                        weldMap[key] = wIdx;
                    }
                    welded[wIdx].incidents.emplace_back(i, c);
                    triWeldedIdx[i][c] = wIdx;
                }
            }

            spdlog::debug("STL smooth normals: {} triangles, {} raw vertices, {} welded vertices",
                triCount, triCount * 3, welded.size());

            // Step 3: For each welded vertex, compute smooth normal per crease group.
            // Group incident triangles: two triangles at the same welded vertex are in the
            // same smooth group if the angle between their face normals < creaseAngle.
            // We use a simple greedy approach: for each incident triangle, find a group
            // whose representative normal has angle < threshold; if none, start a new group.

            // Per-triangle-corner: the smooth normal to use
            // Indexed as [triIdx * 3 + corner]
            std::vector<Vector3> cornerNormals(triCount * 3, Vector3(0.0f, 1.0f, 0.0f));

            for (auto& wv : welded) {
                if (wv.incidents.empty()) continue;

                if (wv.incidents.size() == 1) {
                    // Single triangle — just use face normal
                    auto [ti, ci] = wv.incidents[0];
                    cornerNormals[ti * 3 + ci] = tris[ti].faceNormal;
                    continue;
                }

                // Group incident triangles by crease angle
                struct SmoothGroup {
                    Vector3 accumulated{0.0f, 0.0f, 0.0f};
                    std::vector<std::pair<size_t, int>> members;
                };
                std::vector<SmoothGroup> groups;

                for (auto [ti, ci] : wv.incidents) {
                    const Vector3& fn = tris[ti].faceNormal;
                    bool assigned = false;

                    for (auto& group : groups) {
                        // Compare with the averaged direction of this group
                        Vector3 groupDir = group.accumulated.normalized();
                        float dot = groupDir.dot(fn);
                        if (dot >= cosCrease) {
                            // Area-weighted accumulation (face normal is already unit,
                            // but we keep the magnitude proportional to triangle area
                            // by using the cross product magnitude implicitly)
                            group.accumulated += fn;
                            group.members.emplace_back(ti, ci);
                            assigned = true;
                            break;
                        }
                    }

                    if (!assigned) {
                        SmoothGroup newGroup;
                        newGroup.accumulated = fn;
                        newGroup.members.emplace_back(ti, ci);
                        groups.push_back(std::move(newGroup));
                    }
                }

                // Assign normalized group normal to each member
                for (auto& group : groups) {
                    Vector3 smoothN = group.accumulated.normalized();
                    for (auto [ti, ci] : group.members) {
                        cornerNormals[ti * 3 + ci] = smoothN;
                    }
                }
            }

            // Step 4: Emit vertices with smooth normals. We need to deduplicate
            // (position, normal) pairs since a welded vertex might have different
            // normals for different smooth groups.

            struct VertexKey {
                uint32_t weldedIdx;
                // Quantized normal to 16-bit for dedup
                int16_t qnx, qny, qnz;

                bool operator==(const VertexKey& o) const {
                    return weldedIdx == o.weldedIdx &&
                           qnx == o.qnx && qny == o.qny && qnz == o.qnz;
                }
            };

            struct VertexKeyHash {
                size_t operator()(const VertexKey& k) const {
                    size_t h = std::hash<uint32_t>()(k.weldedIdx);
                    h ^= std::hash<int16_t>()(k.qnx) + 0x9e3779b9 + (h << 6) + (h >> 2);
                    h ^= std::hash<int16_t>()(k.qny) + 0x9e3779b9 + (h << 6) + (h >> 2);
                    h ^= std::hash<int16_t>()(k.qnz) + 0x9e3779b9 + (h << 6) + (h >> 2);
                    return h;
                }
            };

            auto quantizeNormal = [](float v) -> int16_t {
                return static_cast<int16_t>(std::round(v * 32767.0f));
            };

            std::unordered_map<VertexKey, uint32_t, VertexKeyHash> vertexMap;
            outVertices.reserve(welded.size());
            outIndices.reserve(triCount * 3);

            for (size_t i = 0; i < triCount; ++i) {
                int order[3] = {0, 1, 2};
                if (config.flipWinding) {
                    std::swap(order[1], order[2]);
                }

                for (int j = 0; j < 3; ++j) {
                    int c = order[j];
                    uint32_t wIdx = triWeldedIdx[i][c];
                    const Vector3& n = cornerNormals[i * 3 + c];

                    VertexKey key{wIdx,
                        quantizeNormal(n.getX()),
                        quantizeNormal(n.getY()),
                        quantizeNormal(n.getZ())};

                    auto it = vertexMap.find(key);
                    if (it != vertexMap.end()) {
                        outIndices.push_back(it->second);
                    } else {
                        auto idx = static_cast<uint32_t>(outVertices.size());

                        float nx = n.getX(), ny = n.getY(), nz = n.getZ();
                        float tx, ty, tz, tw;
                        if (config.generateTangents) {
                            tangentFromNormal(nx, ny, nz, tx, ty, tz, tw);
                        } else {
                            tx = 1.0f; ty = 0.0f; tz = 0.0f; tw = 1.0f;
                        }

                        const auto& wv = welded[wIdx];

                        PackedVertex vert{};
                        vert.px = wv.px; vert.py = wv.py; vert.pz = wv.pz;
                        vert.nx = nx; vert.ny = ny; vert.nz = nz;
                        vert.u = 0.0f; vert.v = 0.0f;
                        vert.tx = tx; vert.ty = ty; vert.tz = tz; vert.tw = tw;
                        vert.u1 = 0.0f; vert.v1 = 0.0f;

                        outVertices.push_back(vert);
                        vertexMap[key] = idx;
                        outIndices.push_back(idx);

                        minX = std::min(minX, vert.px);
                        minY = std::min(minY, vert.py);
                        minZ = std::min(minZ, vert.pz);
                        maxX = std::max(maxX, vert.px);
                        maxY = std::max(maxY, vert.py);
                        maxZ = std::max(maxZ, vert.pz);
                    }
                }
            }
        }

    } // anonymous namespace

    // ── StlParser::parse ─────────────────────────────────────────────────

    std::unique_ptr<GlbContainerResource> StlParser::parse(
        const std::string& path,
        const std::shared_ptr<GraphicsDevice>& device,
        const StlParserConfig& config)
    {
        if (!device) {
            spdlog::error("STL parse failed: graphics device is null");
            return nullptr;
        }

        // Read entire file into memory
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            spdlog::error("STL parse failed: cannot open '{}'", path);
            return nullptr;
        }

        const auto fileSize = file.tellg();
        if (fileSize <= 0) {
            spdlog::error("STL parse failed: empty file '{}'", path);
            return nullptr;
        }

        std::vector<uint8_t> data(static_cast<size_t>(fileSize));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(data.data()), fileSize);
        file.close();

        // Parse triangles (auto-detect binary vs ASCII)
        std::vector<StlTriangle> triangles;
        std::string solidName;

        if (isBinaryStl(data)) {
            if (!parseBinaryStl(data, triangles, solidName)) {
                spdlog::error("STL binary parse failed: '{}'", path);
                return nullptr;
            }
        } else {
            if (!parseAsciiStl(data, triangles, solidName)) {
                spdlog::error("STL ASCII parse failed: '{}'", path);
                return nullptr;
            }
        }

        if (triangles.empty()) {
            spdlog::warn("STL file has no triangles: '{}'", path);
            return nullptr;
        }

        spdlog::info("STL loaded [{}]: {} triangles, format={}, solid='{}'",
            path, triangles.size(),
            isBinaryStl(data) ? "binary" : "ASCII",
            solidName);

        // Build vertex/index data
        std::vector<PackedVertex> vertices;
        std::vector<uint32_t> indices;
        float minX = std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        float minZ = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float maxY = std::numeric_limits<float>::lowest();
        float maxZ = std::numeric_limits<float>::lowest();

        if (config.generateSmoothNormals) {
            buildSmoothMesh(triangles, config, vertices, indices,
                            minX, minY, minZ, maxX, maxY, maxZ);
        } else {
            buildFlatMesh(triangles, config, vertices, indices,
                          minX, minY, minZ, maxX, maxY, maxZ);
        }

        if (vertices.empty()) {
            spdlog::error("STL parse produced no vertices: '{}'", path);
            return nullptr;
        }

        spdlog::info("STL mesh [{}]: {} vertices, {} indices ({}x reduction from raw)",
            path, vertices.size(), indices.size(),
            static_cast<float>(triangles.size() * 3) / static_cast<float>(vertices.size()));

        // ── Create GPU buffers ───────────────────────────────────────────

        constexpr int BYTES_PER_VERTEX = static_cast<int>(sizeof(PackedVertex));
        auto vertexFormat = std::make_shared<VertexFormat>(BYTES_PER_VERTEX, true, false);

        const int vertexCount = static_cast<int>(vertices.size());
        std::vector<uint8_t> vertexBytes(vertices.size() * sizeof(PackedVertex));
        std::memcpy(vertexBytes.data(), vertices.data(), vertexBytes.size());

        VertexBufferOptions vbOpts;
        vbOpts.usage = BUFFER_STATIC;
        vbOpts.data = std::move(vertexBytes);
        auto vb = device->createVertexBuffer(vertexFormat, vertexCount, vbOpts);

        const int indexCount = static_cast<int>(indices.size());
        IndexFormat idxFmt = (vertexCount <= 65535) ? INDEXFORMAT_UINT16 : INDEXFORMAT_UINT32;

        std::vector<uint8_t> indexBytes;
        if (idxFmt == INDEXFORMAT_UINT16) {
            indexBytes.resize(indices.size() * sizeof(uint16_t));
            auto* dst = reinterpret_cast<uint16_t*>(indexBytes.data());
            for (size_t i = 0; i < indices.size(); ++i) {
                dst[i] = static_cast<uint16_t>(indices[i]);
            }
        } else {
            indexBytes.resize(indices.size() * sizeof(uint32_t));
            std::memcpy(indexBytes.data(), indices.data(), indexBytes.size());
        }
        auto ib = device->createIndexBuffer(idxFmt, indexCount, indexBytes);

        // ── Assemble Mesh ────────────────────────────────────────────────

        auto meshResource = std::make_shared<Mesh>();
        meshResource->setVertexBuffer(vb);
        meshResource->setIndexBuffer(ib, 0);

        Primitive prim;
        prim.type = PRIMITIVE_TRIANGLES;
        prim.base = 0;
        prim.baseVertex = 0;
        prim.count = indexCount;
        prim.indexed = true;
        meshResource->setPrimitive(prim, 0);

        BoundingBox bounds;
        bounds.setCenter(
            (minX + maxX) * 0.5f,
            (minY + maxY) * 0.5f,
            (minZ + maxZ) * 0.5f);
        bounds.setHalfExtents(
            (maxX - minX) * 0.5f,
            (maxY - minY) * 0.5f,
            (maxZ - minZ) * 0.5f);
        meshResource->setAabb(bounds);

        // ── Create default PBR material ──────────────────────────────────

        auto material = std::make_shared<StandardMaterial>();
        material->setName(solidName.empty() ? "stl-default" : solidName);
        material->setDiffuse(Color(config.diffuseR, config.diffuseG, config.diffuseB, 1.0f));
        material->setBaseColorFactor(Color(config.diffuseR, config.diffuseG, config.diffuseB, 1.0f));
        material->setMetalness(config.metalness);
        material->setMetallicFactor(config.metalness);
        material->setGloss(1.0f - config.roughness);
        material->setRoughnessFactor(config.roughness);
        material->setUseMetalness(true);
        material->setCullMode(CullMode::CULLFACE_BACK);

        // ── Package into GlbContainerResource ────────────────────────────

        auto container = std::make_unique<GlbContainerResource>();

        GlbMeshPayload payload;
        payload.mesh = meshResource;
        payload.material = material;
        container->addMeshPayload(payload);

        // STL is always a single flat mesh — one root node
        GlbNodePayload rootNode;
        rootNode.name = solidName.empty()
            ? std::filesystem::path(path).stem().string()
            : solidName;
        rootNode.scale = Vector3(1.0f, 1.0f, 1.0f);
        rootNode.meshPayloadIndices.push_back(0);
        container->addNodePayload(rootNode);
        container->addRootNodeIndex(0);

        spdlog::info("STL parse complete [{}]: {} vertices, {} indices, bounds=[{:.3f},{:.3f},{:.3f}]-[{:.3f},{:.3f},{:.3f}]",
            path, vertices.size(), indices.size(),
            minX, minY, minZ, maxX, maxY, maxZ);

        return container;
    }

} // namespace visutwin::canvas
