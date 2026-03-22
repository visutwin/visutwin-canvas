// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// OBJ + MTL file parser for VisuTwin Canvas.
//
// Uses tinyobjloader (v2 ObjReader API) to parse Wavefront OBJ geometry and
// companion MTL material definitions. Produces a GlbContainerResource with
// the engine's standard 14-float interleaved vertex layout (matching GlbParser).
//
// Key design decisions:
//   - Reuses GlbContainerResource for unified instantiateRenderEntity() path
//   - Material-based sub-mesh splitting (one draw call per material per shape)
//   - Vertex deduplication via (position, normal, texcoord) index hash
//   - Phong-to-PBR material conversion for classic MTL files
//   - Smooth normal generation when OBJ has no normals
//   - Tangent generation from UV seams (Lengyel algorithm, same as GlbParser)
//
// Custom loader (not derived from upstream).
//
#include "objParser.h"

#include <tiny_obj_loader.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <unordered_map>
#include <vector>

#include "core/math/vector3.h"
#include "core/math/vector4.h"
#include "core/shape/boundingBox.h"
#include "platform/graphics/constants.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/materials/standardMaterial.h"
#include "spdlog/spdlog.h"
#include "stb_image.h"

namespace visutwin::canvas
{
    namespace
    {
        // ── Vertex layout (must match GlbParser::PackedVertex) ──────────

        struct PackedVertex
        {
            float px, py, pz;       // position
            float nx, ny, nz;       // normal
            float u, v;             // uv0
            float tx, ty, tz, tw;   // tangent + handedness
            float u1, v1;           // uv1
        };

        static_assert(sizeof(PackedVertex) == 56, "PackedVertex must be 56 bytes (14 floats)");

        // ── Vertex deduplication key ────────────────────────────────────

        struct VertexKey
        {
            int vi, ni, ti;  // position, normal, texcoord indices

            bool operator==(const VertexKey& o) const
            {
                return vi == o.vi && ni == o.ni && ti == o.ti;
            }
        };

        struct VertexKeyHash
        {
            size_t operator()(const VertexKey& k) const
            {
                size_t h = std::hash<int>()(k.vi);
                h ^= std::hash<int>()(k.ni) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<int>()(k.ti) + 0x9e3779b9 + (h << 6) + (h >> 2);
                return h;
            }
        };

        // ── Tangent generation (same algorithm as GlbParser) ────────────

        void generateTangents(std::vector<PackedVertex>& vertices, const std::vector<uint32_t>& indices)
        {
            const size_t vertexCount = vertices.size();
            if (vertexCount == 0) return;

            std::vector<Vector3> tan1(vertexCount, Vector3(0.0f, 0.0f, 0.0f));
            std::vector<Vector3> tan2(vertexCount, Vector3(0.0f, 0.0f, 0.0f));

            auto accumulateTriangle = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
                if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) return;

                const auto& v0 = vertices[i0];
                const auto& v1 = vertices[i1];
                const auto& v2 = vertices[i2];

                const float du1 = v1.u - v0.u;
                const float dv1 = v1.v - v0.v;
                const float du2 = v2.u - v0.u;
                const float dv2 = v2.v - v0.v;

                const float det = du1 * dv2 - dv1 * du2;
                if (std::abs(det) <= 1e-8f) return;

                const float invDet = 1.0f / det;
                const Vector3 e1(v1.px - v0.px, v1.py - v0.py, v1.pz - v0.pz);
                const Vector3 e2(v2.px - v0.px, v2.py - v0.py, v2.pz - v0.pz);

                const Vector3 sdir = (e1 * dv2 - e2 * dv1) * invDet;
                const Vector3 tdir = (e2 * du1 - e1 * du2) * invDet;

                tan1[i0] += sdir; tan1[i1] += sdir; tan1[i2] += sdir;
                tan2[i0] += tdir; tan2[i1] += tdir; tan2[i2] += tdir;
            };

            if (!indices.empty()) {
                for (size_t i = 0; i + 2 < indices.size(); i += 3)
                    accumulateTriangle(indices[i], indices[i + 1], indices[i + 2]);
            } else {
                for (uint32_t i = 0; i + 2 < static_cast<uint32_t>(vertexCount); i += 3)
                    accumulateTriangle(i, i + 1, i + 2);
            }

            for (size_t i = 0; i < vertexCount; ++i) {
                const Vector3 n(vertices[i].nx, vertices[i].ny, vertices[i].nz);
                Vector3 t = tan1[i] - n * n.dot(tan1[i]);
                if (t.lengthSquared() <= 1e-8f) {
                    t = std::abs(n.getY()) < 0.999f
                        ? n.cross(Vector3(0.0f, 1.0f, 0.0f))
                        : n.cross(Vector3(1.0f, 0.0f, 0.0f));
                }
                t = t.normalized();

                const float handedness = (n.cross(t).dot(tan2[i]) < 0.0f) ? -1.0f : 1.0f;

                vertices[i].tx = t.getX();
                vertices[i].ty = t.getY();
                vertices[i].tz = t.getZ();
                vertices[i].tw = handedness;
            }
        }

        // ── Tangent-from-normal fallback (no UVs available) ─────────────

        void tangentFromNormal(float nx, float ny, float nz,
                               float& tx, float& ty, float& tz, float& tw)
        {
            // Gram-Schmidt cross product to get an arbitrary tangent perpendicular to normal
            Vector3 n(nx, ny, nz);
            Vector3 up = std::abs(ny) < 0.999f ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
            Vector3 t = n.cross(up).normalized();
            tx = t.getX();
            ty = t.getY();
            tz = t.getZ();
            tw = 1.0f;
        }

        // ── Smooth normal generation ────────────────────────────────────

        void generateSmoothNormals(
            const tinyobj::attrib_t& attrib,
            const tinyobj::mesh_t& mesh,
            std::vector<float>& outNormals)  // 3 floats per face-vertex (same count as mesh.indices)
        {
            const size_t totalFaceVertices = mesh.indices.size();
            outNormals.resize(totalFaceVertices * 3, 0.0f);

            // Accumulate area-weighted face normals per vertex per smoothing group
            // Key: (vertex_index, smoothing_group) -> accumulated normal
            struct SmoothKey {
                int vertexIndex;
                unsigned int smoothingGroup;
                bool operator==(const SmoothKey& o) const {
                    return vertexIndex == o.vertexIndex && smoothingGroup == o.smoothingGroup;
                }
            };
            struct SmoothKeyHash {
                size_t operator()(const SmoothKey& k) const {
                    size_t h = std::hash<int>()(k.vertexIndex);
                    h ^= std::hash<unsigned int>()(k.smoothingGroup) + 0x9e3779b9 + (h << 6) + (h >> 2);
                    return h;
                }
            };

            std::unordered_map<SmoothKey, Vector3, SmoothKeyHash> accum;

            // Pre-compute face offsets for O(1) access
            const size_t faceCount = mesh.num_face_vertices.size();
            std::vector<size_t> faceOffsets(faceCount);
            {
                size_t offset = 0;
                for (size_t f = 0; f < faceCount; ++f) {
                    faceOffsets[f] = offset;
                    offset += mesh.num_face_vertices[f];
                }
            }

            // Pass 1: accumulate face normals
            for (size_t f = 0; f < faceCount; ++f) {
                const unsigned int fv = mesh.num_face_vertices[f];
                if (fv < 3) continue;

                const size_t base = faceOffsets[f];
                const unsigned int sg = (f < mesh.smoothing_group_ids.size())
                    ? mesh.smoothing_group_ids[f] : 0;

                // Compute face normal from first 3 vertices
                const auto& i0 = mesh.indices[base + 0];
                const auto& i1 = mesh.indices[base + 1];
                const auto& i2 = mesh.indices[base + 2];

                Vector3 p0(attrib.vertices[3 * i0.vertex_index + 0],
                           attrib.vertices[3 * i0.vertex_index + 1],
                           attrib.vertices[3 * i0.vertex_index + 2]);
                Vector3 p1(attrib.vertices[3 * i1.vertex_index + 0],
                           attrib.vertices[3 * i1.vertex_index + 1],
                           attrib.vertices[3 * i1.vertex_index + 2]);
                Vector3 p2(attrib.vertices[3 * i2.vertex_index + 0],
                           attrib.vertices[3 * i2.vertex_index + 1],
                           attrib.vertices[3 * i2.vertex_index + 2]);

                Vector3 faceNormal = (p1 - p0).cross(p2 - p0);
                // Area-weighted (un-normalized cross product magnitude ~ 2x triangle area)

                if (sg == 0) {
                    // Flat shading: assign face normal directly to each vertex
                    float len = faceNormal.length();
                    if (len > 1e-8f) {
                        faceNormal = faceNormal * (1.0f / len);
                    } else {
                        faceNormal = Vector3(0.0f, 1.0f, 0.0f);
                    }
                    for (unsigned int j = 0; j < fv; ++j) {
                        outNormals[(base + j) * 3 + 0] = faceNormal.getX();
                        outNormals[(base + j) * 3 + 1] = faceNormal.getY();
                        outNormals[(base + j) * 3 + 2] = faceNormal.getZ();
                    }
                } else {
                    // Smooth shading: accumulate for later normalization
                    for (unsigned int j = 0; j < fv; ++j) {
                        int vi = mesh.indices[base + j].vertex_index;
                        SmoothKey key{vi, sg};
                        accum[key] += faceNormal;
                    }
                }
            }

            // Pass 2: normalize accumulated normals and write to output
            for (size_t f = 0; f < faceCount; ++f) {
                const unsigned int fv = mesh.num_face_vertices[f];
                const size_t base = faceOffsets[f];
                const unsigned int sg = (f < mesh.smoothing_group_ids.size())
                    ? mesh.smoothing_group_ids[f] : 0;

                if (sg == 0) continue;  // already written in pass 1

                for (unsigned int j = 0; j < fv; ++j) {
                    int vi = mesh.indices[base + j].vertex_index;
                    SmoothKey key{vi, sg};
                    auto it = accum.find(key);
                    if (it != accum.end()) {
                        Vector3 n = it->second.normalized();
                        outNormals[(base + j) * 3 + 0] = n.getX();
                        outNormals[(base + j) * 3 + 1] = n.getY();
                        outNormals[(base + j) * 3 + 2] = n.getZ();
                    } else {
                        outNormals[(base + j) * 3 + 0] = 0.0f;
                        outNormals[(base + j) * 3 + 1] = 1.0f;
                        outNormals[(base + j) * 3 + 2] = 0.0f;
                    }
                }
            }
        }

        // ── Texture loading ─────────────────────────────────────────────

        std::shared_ptr<Texture> loadObjTexture(
            GraphicsDevice* device,
            const std::string& texname,
            const std::filesystem::path& basedir,
            std::unordered_map<std::string, std::shared_ptr<Texture>>& cache)
        {
            if (texname.empty()) return nullptr;

            auto it = cache.find(texname);
            if (it != cache.end()) return it->second;

            auto texPath = basedir / texname;
            if (!std::filesystem::exists(texPath)) {
                spdlog::warn("OBJ texture not found: {}", texPath.string());
                cache[texname] = nullptr;
                return nullptr;
            }

            // OBJ textures typically use bottom-left origin (OpenGL convention)
            stbi_set_flip_vertically_on_load(true);
            int w, h, comp;
            stbi_uc* pixels = stbi_load(texPath.string().c_str(), &w, &h, &comp, 4);
            if (!pixels) {
                spdlog::warn("OBJ texture decode failed: {}", texPath.string());
                cache[texname] = nullptr;
                return nullptr;
            }

            TextureOptions opts;
            opts.width = static_cast<uint32_t>(w);
            opts.height = static_cast<uint32_t>(h);
            opts.format = PixelFormat::PIXELFORMAT_RGBA8;
            opts.mipmaps = false;
            opts.numLevels = 1;
            opts.minFilter = FilterMode::FILTER_LINEAR;
            opts.magFilter = FilterMode::FILTER_LINEAR;
            opts.name = texname;

            auto texture = std::make_shared<Texture>(device, opts);
            const size_t dataSize = static_cast<size_t>(w) * h * 4;
            texture->setLevelData(0, pixels, dataSize);
            stbi_image_free(pixels);
            texture->upload();

            spdlog::info("OBJ texture loaded: {} ({}x{})", texname, w, h);
            cache[texname] = texture;
            return texture;
        }

        // ── Phong-to-PBR material conversion ────────────────────────────

        float roughnessFromShininess(float ns)
        {
            ns = std::clamp(ns, 0.0f, 1000.0f);
            return 1.0f - std::sqrt(ns / 1000.0f);
        }

        bool hasPbrData(const tinyobj::material_t& mat)
        {
            return mat.roughness > 0.0f || mat.metallic > 0.0f ||
                   !mat.roughness_texname.empty() || !mat.metallic_texname.empty() ||
                   !mat.normal_texname.empty();
        }

        std::shared_ptr<StandardMaterial> convertMtlMaterial(
            const tinyobj::material_t& mtl,
            GraphicsDevice* device,
            const std::filesystem::path& basedir,
            std::unordered_map<std::string, std::shared_ptr<Texture>>& texCache,
            std::vector<std::shared_ptr<Texture>>& ownedTextures)
        {
            auto material = std::make_shared<StandardMaterial>();
            material->setName(mtl.name.empty() ? "obj-material" : mtl.name);

            const bool usePbr = hasPbrData(mtl);

            if (usePbr) {
                // Direct PBR path
                Color baseColor(mtl.diffuse[0], mtl.diffuse[1], mtl.diffuse[2], mtl.dissolve);
                material->setDiffuse(baseColor);
                material->setBaseColorFactor(baseColor);
                material->setMetalness(mtl.metallic);
                material->setMetallicFactor(mtl.metallic);
                material->setGloss(1.0f - mtl.roughness);
                material->setRoughnessFactor(mtl.roughness);
            } else {
                // Phong -> PBR conversion
                Color diffuse(mtl.diffuse[0], mtl.diffuse[1], mtl.diffuse[2], mtl.dissolve);
                material->setDiffuse(diffuse);
                material->setBaseColorFactor(diffuse);
                material->setSpecular(Color(mtl.specular[0], mtl.specular[1], mtl.specular[2], 1.0f));

                float roughness = roughnessFromShininess(mtl.shininess);
                material->setGloss(1.0f - roughness);
                material->setRoughnessFactor(roughness);

                // Metallic heuristic: if diffuse is very dark and specular is bright + colored -> metallic
                float kdLum = 0.2126f * mtl.diffuse[0] + 0.7152f * mtl.diffuse[1] + 0.0722f * mtl.diffuse[2];
                float ksLum = 0.2126f * mtl.specular[0] + 0.7152f * mtl.specular[1] + 0.0722f * mtl.specular[2];
                float metallic = 0.0f;
                if (kdLum < 0.04f && ksLum > 0.5f) {
                    metallic = 1.0f;
                } else if (ksLum > 0.25f) {
                    float ksMax = std::max({mtl.specular[0], mtl.specular[1], mtl.specular[2]});
                    float ksMin = std::min({mtl.specular[0], mtl.specular[1], mtl.specular[2]});
                    float sat = (ksMax > 0.001f) ? (ksMax - ksMin) / ksMax : 0.0f;
                    if (sat > 0.2f) metallic = 0.8f;
                }
                material->setMetalness(metallic);
                material->setMetallicFactor(metallic);
            }

            material->setUseMetalness(true);
            material->setOpacity(mtl.dissolve);

            // Emissive
            if (mtl.emission[0] > 0.0f || mtl.emission[1] > 0.0f || mtl.emission[2] > 0.0f) {
                material->setEmissive(Color(mtl.emission[0], mtl.emission[1], mtl.emission[2], 1.0f));
                material->setEmissiveFactor(Color(mtl.emission[0], mtl.emission[1], mtl.emission[2], 1.0f));
            }

            // Alpha mode
            if (mtl.dissolve < 0.99f) {
                material->setAlphaMode(AlphaMode::BLEND);
                material->setTransparent(true);
            }

            material->setCullMode(CullMode::CULLFACE_BACK);

            // ── Texture loading ─────────────────────────────────────────

            auto loadAndOwn = [&](const std::string& name) -> Texture* {
                auto tex = loadObjTexture(device, name, basedir, texCache);
                if (tex) ownedTextures.push_back(tex);
                return tex.get();
            };

            // Diffuse / base color map
            if (auto* tex = loadAndOwn(mtl.diffuse_texname)) {
                material->setDiffuseMap(tex);
                material->setBaseColorTexture(tex);
                material->setHasBaseColorTexture(true);
            }

            // Normal map (prefer PBR norm, fall back to bump)
            std::string normalTexName = mtl.normal_texname.empty() ? mtl.bump_texname : mtl.normal_texname;
            if (auto* tex = loadAndOwn(normalTexName)) {
                material->setNormalMap(tex);
                material->setNormalTexture(tex);
                material->setHasNormalTexture(true);
                if (!mtl.bump_texname.empty() && mtl.normal_texname.empty()) {
                    material->setBumpiness(mtl.bump_texopt.bump_multiplier);
                    material->setNormalScale(mtl.bump_texopt.bump_multiplier);
                }
            }

            // AO map (map_Ka used as ambient occlusion)
            if (auto* tex = loadAndOwn(mtl.ambient_texname)) {
                material->setAoMap(tex);
                material->setOcclusionTexture(tex);
                material->setHasOcclusionTexture(true);
            }

            // Emissive map
            if (auto* tex = loadAndOwn(mtl.emissive_texname)) {
                material->setEmissiveMap(tex);
                material->setEmissiveTexture(tex);
                material->setHasEmissiveTexture(true);
            }

            // Opacity map
            if (auto* tex = loadAndOwn(mtl.alpha_texname)) {
                material->setOpacityMap(tex);
                material->setAlphaMode(AlphaMode::MASK);
            }

            return material;
        }

    } // anonymous namespace

    // ── ObjParser::parse ────────────────────────────────────────────────

    std::unique_ptr<GlbContainerResource> ObjParser::parse(
        const std::string& path,
        const std::shared_ptr<GraphicsDevice>& device,
        const ObjParserConfig& config)
    {
        if (!device) {
            spdlog::error("OBJ parse failed: graphics device is null");
            return nullptr;
        }

        // Resolve base directory for MTL and texture file lookup
        const std::filesystem::path objPath(path);
        const std::filesystem::path basedir = config.mtlSearchPath.empty()
            ? objPath.parent_path()
            : std::filesystem::path(config.mtlSearchPath);

        // Parse with tinyobjloader v2 API
        tinyobj::ObjReaderConfig readerConfig;
        readerConfig.triangulate = true;
        readerConfig.vertex_color = true;
        readerConfig.mtl_search_path = basedir.string();

        tinyobj::ObjReader reader;
        if (!reader.ParseFromFile(path, readerConfig)) {
            spdlog::error("OBJ parse failed [{}]: {}", path, reader.Error());
            return nullptr;
        }
        if (!reader.Warning().empty()) {
            spdlog::warn("OBJ parse warning [{}]: {}", path, reader.Warning());
        }

        const auto& attrib = reader.GetAttrib();
        const auto& shapes = reader.GetShapes();
        const auto& materials = reader.GetMaterials();

        const bool hasNormals = !attrib.normals.empty();
        const bool hasTexcoords = !attrib.texcoords.empty();

        spdlog::info("OBJ loaded [{}]: {} vertices, {} normals, {} texcoords, "
                     "{} shapes, {} materials",
            path,
            attrib.vertices.size() / 3,
            attrib.normals.size() / 3,
            attrib.texcoords.size() / 2,
            shapes.size(),
            materials.size());

        // ── Convert materials ───────────────────────────────────────────

        auto container = std::make_unique<GlbContainerResource>();
        std::unordered_map<std::string, std::shared_ptr<Texture>> texCache;
        std::vector<std::shared_ptr<Texture>> ownedTextures;

        std::vector<std::shared_ptr<Material>> objMaterials;
        if (materials.empty()) {
            // Default gray material when no MTL file
            auto mat = std::make_shared<StandardMaterial>();
            mat->setName("obj-default");
            mat->setDiffuse(Color(0.8f, 0.8f, 0.8f, 1.0f));
            mat->setBaseColorFactor(Color(0.8f, 0.8f, 0.8f, 1.0f));
            mat->setMetalness(0.0f);
            mat->setMetallicFactor(0.0f);
            mat->setGloss(0.5f);
            mat->setRoughnessFactor(0.5f);
            mat->setUseMetalness(true);
            objMaterials.push_back(mat);
        } else {
            for (const auto& mtl : materials) {
                objMaterials.push_back(
                    convertMtlMaterial(mtl, device.get(), basedir, texCache, ownedTextures));
            }
        }

        // ── Process shapes ──────────────────────────────────────────────

        constexpr int BYTES_PER_VERTEX = static_cast<int>(sizeof(PackedVertex));  // 56
        auto vertexFormat = std::make_shared<VertexFormat>(BYTES_PER_VERTEX, true, false);

        size_t meshPayloadIndex = 0;

        for (size_t shapeIdx = 0; shapeIdx < shapes.size(); ++shapeIdx) {
            const auto& shape = shapes[shapeIdx];
            const auto& mesh = shape.mesh;

            // Pre-compute face offsets (prefix sum) for O(1) access
            const size_t faceCount = mesh.num_face_vertices.size();
            std::vector<size_t> faceOffsets(faceCount);
            {
                size_t offset = 0;
                for (size_t f = 0; f < faceCount; ++f) {
                    faceOffsets[f] = offset;
                    offset += mesh.num_face_vertices[f];
                }
            }

            // Generate smooth normals if OBJ has none
            std::vector<float> generatedNormals;
            if (!hasNormals && config.generateNormals) {
                generateSmoothNormals(attrib, mesh, generatedNormals);
            }

            // Group faces by material_id
            std::unordered_map<int, std::vector<size_t>> materialFaceGroups;
            for (size_t f = 0; f < faceCount; ++f) {
                int matId = (f < mesh.material_ids.size()) ? mesh.material_ids[f] : -1;
                if (matId < 0 || matId >= static_cast<int>(objMaterials.size())) matId = 0;
                materialFaceGroups[matId].push_back(f);
            }

            // For each material group, build vertex/index buffers
            for (auto& [matId, faceIndices] : materialFaceGroups) {
                std::unordered_map<VertexKey, uint32_t, VertexKeyHash> vertexMap;
                std::vector<PackedVertex> vertices;
                std::vector<uint32_t> indices;
                float minX = std::numeric_limits<float>::max();
                float minY = std::numeric_limits<float>::max();
                float minZ = std::numeric_limits<float>::max();
                float maxX = std::numeric_limits<float>::lowest();
                float maxY = std::numeric_limits<float>::lowest();
                float maxZ = std::numeric_limits<float>::lowest();

                for (size_t fi : faceIndices) {
                    const size_t base = faceOffsets[fi];
                    const unsigned int fv = mesh.num_face_vertices[fi];

                    for (unsigned int j = 0; j < fv; ++j) {
                        const size_t idx = base + j;
                        const auto& objIdx = mesh.indices[idx];

                        // Position
                        float px = attrib.vertices[3 * objIdx.vertex_index + 0];
                        float py = attrib.vertices[3 * objIdx.vertex_index + 1];
                        float pz = attrib.vertices[3 * objIdx.vertex_index + 2];

                        // Apply config transforms
                        px *= config.uniformScale;
                        py *= config.uniformScale;
                        pz *= config.uniformScale;
                        if (config.flipYZ) {
                            std::swap(py, pz);
                            pz = -pz;
                        }

                        // Normal
                        float nx, ny, nz;
                        if (hasNormals && objIdx.normal_index >= 0) {
                            nx = attrib.normals[3 * objIdx.normal_index + 0];
                            ny = attrib.normals[3 * objIdx.normal_index + 1];
                            nz = attrib.normals[3 * objIdx.normal_index + 2];
                            if (config.flipYZ) {
                                std::swap(ny, nz);
                                nz = -nz;
                            }
                            // Re-normalize
                            float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                            if (len > 1e-8f) {
                                float inv = 1.0f / len;
                                nx *= inv; ny *= inv; nz *= inv;
                            }
                        } else if (!generatedNormals.empty()) {
                            nx = generatedNormals[idx * 3 + 0];
                            ny = generatedNormals[idx * 3 + 1];
                            nz = generatedNormals[idx * 3 + 2];
                            if (config.flipYZ) {
                                std::swap(ny, nz);
                                nz = -nz;
                            }
                        } else {
                            nx = 0.0f; ny = 1.0f; nz = 0.0f;
                        }

                        // Texcoord
                        float u = 0.0f, vt = 0.0f;
                        if (hasTexcoords && objIdx.texcoord_index >= 0) {
                            u = attrib.texcoords[2 * objIdx.texcoord_index + 0];
                            vt = attrib.texcoords[2 * objIdx.texcoord_index + 1];
                            // Metal uses top-left origin; OBJ uses bottom-left
                            vt = 1.0f - vt;
                        }

                        // Vertex deduplication
                        VertexKey key{objIdx.vertex_index, objIdx.normal_index, objIdx.texcoord_index};
                        auto it = vertexMap.find(key);
                        if (it != vertexMap.end()) {
                            indices.push_back(it->second);
                        } else {
                            uint32_t newIdx = static_cast<uint32_t>(vertices.size());

                            // Tangent placeholder (will be overwritten by generateTangents if UVs exist)
                            float tx, ty, tz, tw;
                            tangentFromNormal(nx, ny, nz, tx, ty, tz, tw);

                            PackedVertex vert{};
                            vert.px = px; vert.py = py; vert.pz = pz;
                            vert.nx = nx; vert.ny = ny; vert.nz = nz;
                            vert.u = u;   vert.v = vt;
                            vert.tx = tx; vert.ty = ty; vert.tz = tz; vert.tw = tw;
                            vert.u1 = u;  vert.v1 = vt;  // UV1 = UV0 for OBJ

                            vertices.push_back(vert);
                            vertexMap[key] = newIdx;
                            indices.push_back(newIdx);

                            minX = std::min(minX, px); minY = std::min(minY, py); minZ = std::min(minZ, pz);
                            maxX = std::max(maxX, px); maxY = std::max(maxY, py); maxZ = std::max(maxZ, pz);
                        }
                    }

                    // Flip winding if requested (triangulated, so always 3 verts)
                    if (config.flipWinding && fv == 3) {
                        size_t last = indices.size();
                        std::swap(indices[last - 2], indices[last - 1]);
                    }
                }

                if (vertices.empty()) continue;

                // Generate proper tangents from UVs if available
                if (hasTexcoords && config.generateTangents) {
                    generateTangents(vertices, indices);
                }

                // ── Create GPU buffers ──────────────────────────────────

                const int vertexCount = static_cast<int>(vertices.size());
                std::vector<uint8_t> vertexBytes(vertices.size() * sizeof(PackedVertex));
                std::memcpy(vertexBytes.data(), vertices.data(), vertexBytes.size());

                VertexBufferOptions vbOpts;
                vbOpts.usage = BUFFER_STATIC;
                vbOpts.data = std::move(vertexBytes);
                auto vb = device->createVertexBuffer(vertexFormat, vertexCount, vbOpts);

                const int indexCount = static_cast<int>(indices.size());
                // Use uint16 for small meshes, uint32 for large
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

                // ── Add payload ─────────────────────────────────────────

                GlbMeshPayload payload;
                payload.mesh = meshResource;
                int safeMat = std::clamp(matId, 0, static_cast<int>(objMaterials.size()) - 1);
                payload.material = objMaterials[static_cast<size_t>(safeMat)];
                container->addMeshPayload(payload);
                ++meshPayloadIndex;
            }
        }

        // ── Build node structure ────────────────────────────────────────
        // OBJ is flat (no hierarchy), so create one node per shape with all
        // its material sub-meshes, or a single root node if only one shape.

        if (shapes.size() == 1) {
            // Single shape: all mesh payloads go into one root node
            GlbNodePayload rootNode;
            rootNode.name = objPath.stem().string();
            rootNode.scale = Vector3(1.0f, 1.0f, 1.0f);
            for (size_t i = 0; i < meshPayloadIndex; ++i) {
                rootNode.meshPayloadIndices.push_back(i);
            }
            container->addNodePayload(rootNode);
            container->addRootNodeIndex(0);
        } else {
            // Multiple shapes: one node per shape, with mesh indices distributed
            size_t payloadCursor = 0;
            for (size_t shapeIdx = 0; shapeIdx < shapes.size(); ++shapeIdx) {
                const auto& shape = shapes[shapeIdx];
                const auto& mesh = shape.mesh;

                // Count how many material groups this shape has
                std::unordered_map<int, bool> matGroups;
                for (size_t f = 0; f < mesh.num_face_vertices.size(); ++f) {
                    int matId = (f < mesh.material_ids.size()) ? mesh.material_ids[f] : 0;
                    if (matId < 0 || matId >= static_cast<int>(objMaterials.size())) matId = 0;
                    matGroups[matId] = true;
                }
                size_t groupCount = matGroups.size();

                GlbNodePayload node;
                node.name = shape.name.empty()
                    ? "shape_" + std::to_string(shapeIdx)
                    : shape.name;
                node.scale = Vector3(1.0f, 1.0f, 1.0f);
                for (size_t i = 0; i < groupCount && payloadCursor < meshPayloadIndex; ++i) {
                    node.meshPayloadIndices.push_back(payloadCursor++);
                }
                container->addNodePayload(node);
                container->addRootNodeIndex(static_cast<int>(shapeIdx));
            }
        }

        // Transfer texture ownership
        for (auto& tex : ownedTextures) {
            container->addOwnedTexture(tex);
        }

        spdlog::info("OBJ parse complete [{}]: {} mesh payloads, {} materials, {} textures",
            path, meshPayloadIndex, objMaterials.size(), ownedTextures.size());

        return container;
    }

} // namespace visutwin::canvas
