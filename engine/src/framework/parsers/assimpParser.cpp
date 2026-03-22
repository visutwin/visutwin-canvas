// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Assimp-based multi-format 3D model parser for VisuTwin Canvas.
//
// Uses Assimp (Open Asset Import Library) to parse Collada (.dae), FBX (.fbx),
// 3DS, PLY, and other formats. Produces a GlbContainerResource with the
// engine's standard 14-float interleaved vertex layout (matching GlbParser
// and ObjParser).
//
// Key design decisions:
//   - Reuses GlbContainerResource for unified instantiateRenderEntity() path
//   - aiProcess_FlipUVs handles Metal's top-left UV origin (no manual V flip)
//   - Two-path material conversion: native PBR (glTF/FBX) and legacy Phong→PBR
//   - Tangent generation: Assimp first, Lengyel fallback, Gram-Schmidt last
//   - Pre-order DFS node traversal maps to GlbNodePayload flat array
//
// Custom loader (not derived from upstream).
//
#include "assimpParser.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/material.h>
#include <assimp/config.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <unordered_map>
#include <vector>

#include "core/math/vector3.h"
#include "core/math/vector4.h"
#include "core/math/quaternion.h"
#include "core/shape/boundingBox.h"
#include "platform/graphics/constants.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/materials/standardMaterial.h"
#include "spdlog/spdlog.h"
#include "stb_image.h"  // declarations only -- STB_IMAGE_IMPLEMENTATION is in asset.cpp

namespace visutwin::canvas
{
    namespace
    {
        // ── Vertex layout (must match GlbParser / ObjParser PackedVertex) ──

        struct PackedVertex
        {
            float px, py, pz;       // position
            float nx, ny, nz;       // normal
            float u, v;             // uv0
            float tx, ty, tz, tw;   // tangent + handedness
            float u1, v1;           // uv1
        };

        static_assert(sizeof(PackedVertex) == 56, "PackedVertex must be 56 bytes (14 floats)");

        // ── Tangent generation (same Lengyel algorithm as ObjParser) ───────

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
            Vector3 n(nx, ny, nz);
            Vector3 up = std::abs(ny) < 0.999f ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
            Vector3 t = n.cross(up).normalized();
            tx = t.getX();
            ty = t.getY();
            tz = t.getZ();
            tw = 1.0f;
        }

        // ── Texture loading (external files + embedded) ────────────────

        std::shared_ptr<Texture> loadAssimpTexture(
            const aiMaterial* aiMat,
            aiTextureType type,
            const aiScene* scene,
            const std::filesystem::path& basedir,
            GraphicsDevice* device,
            std::unordered_map<std::string, std::shared_ptr<Texture>>& cache)
        {
            aiString texPath;
            if (aiGetMaterialTexture(aiMat, type, 0, &texPath) != AI_SUCCESS)
                return nullptr;

            std::string pathStr = texPath.C_Str();
            if (pathStr.empty())
                return nullptr;

            // Check cache
            auto cacheIt = cache.find(pathStr);
            if (cacheIt != cache.end())
                return cacheIt->second;

            int w = 0, h = 0, channels = 0;
            stbi_uc* pixels = nullptr;

            if (pathStr[0] == '*') {
                // Embedded texture: "*N" indexes into scene->mTextures
                int texIndex = std::atoi(pathStr.c_str() + 1);
                if (texIndex < 0 || texIndex >= static_cast<int>(scene->mNumTextures)) {
                    spdlog::warn("Assimp: embedded texture index out of range: {}", pathStr);
                    cache[pathStr] = nullptr;
                    return nullptr;
                }
                const aiTexture* aiTex = scene->mTextures[texIndex];
                if (aiTex->mHeight == 0) {
                    // Compressed format (PNG/JPEG) -- mWidth is byte length
                    stbi_set_flip_vertically_on_load(false);
                    pixels = stbi_load_from_memory(
                        reinterpret_cast<const unsigned char*>(aiTex->pcData),
                        static_cast<int>(aiTex->mWidth),
                        &w, &h, &channels, 4);
                } else {
                    // Uncompressed BGRA (aiTexel: {b, g, r, a})
                    w = static_cast<int>(aiTex->mWidth);
                    h = static_cast<int>(aiTex->mHeight);
                    const size_t pixelCount = static_cast<size_t>(w) * h;
                    pixels = static_cast<stbi_uc*>(malloc(pixelCount * 4));
                    for (size_t i = 0; i < pixelCount; ++i) {
                        const auto& texel = aiTex->pcData[i];
                        pixels[i * 4 + 0] = texel.r;
                        pixels[i * 4 + 1] = texel.g;
                        pixels[i * 4 + 2] = texel.b;
                        pixels[i * 4 + 3] = texel.a;
                    }
                    channels = 4;
                }
            } else {
                // External texture file
                std::filesystem::path fullPath;
                std::filesystem::path texFilePath(pathStr);
                if (texFilePath.is_absolute()) {
                    fullPath = texFilePath;
                } else {
                    fullPath = basedir / texFilePath;
                }
                if (!std::filesystem::exists(fullPath)) {
                    spdlog::warn("Assimp texture not found: {}", fullPath.string());
                    cache[pathStr] = nullptr;
                    return nullptr;
                }
                // UVs already flipped by aiProcess_FlipUVs -- do NOT flip texture data
                stbi_set_flip_vertically_on_load(false);
                pixels = stbi_load(fullPath.string().c_str(), &w, &h, &channels, 4);
            }

            if (!pixels || w <= 0 || h <= 0) {
                spdlog::warn("Assimp texture decode failed: {}", pathStr);
                if (pixels) stbi_image_free(pixels);
                cache[pathStr] = nullptr;
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
            opts.name = pathStr;

            auto texture = std::make_shared<Texture>(device, opts);
            const size_t dataSize = static_cast<size_t>(w) * h * 4;
            texture->setLevelData(0, pixels, dataSize);
            stbi_image_free(pixels);
            texture->upload();

            spdlog::info("Assimp texture loaded: {} ({}x{})", pathStr, w, h);
            cache[pathStr] = texture;
            return texture;
        }

        // ── PBR material conversion (glTF, FBX with PBR) ──────────────

        void convertPbrMaterial(
            const aiMaterial* aiMat,
            const aiScene* scene,
            const std::filesystem::path& basedir,
            GraphicsDevice* device,
            StandardMaterial& material,
            std::unordered_map<std::string, std::shared_ptr<Texture>>& texCache,
            std::vector<std::shared_ptr<Texture>>& ownedTextures)
        {
            auto loadAndOwn = [&](aiTextureType type) -> Texture* {
                auto tex = loadAssimpTexture(aiMat, type, scene, basedir, device, texCache);
                if (tex) ownedTextures.push_back(tex);
                return tex.get();
            };

            // Base color
            aiColor4D baseColor(1.0f, 1.0f, 1.0f, 1.0f);
            aiGetMaterialColor(aiMat, AI_MATKEY_BASE_COLOR, &baseColor);
            material.setDiffuse(Color(baseColor.r, baseColor.g, baseColor.b, baseColor.a));
            material.setBaseColorFactor(Color(baseColor.r, baseColor.g, baseColor.b, baseColor.a));
            material.setOpacity(baseColor.a);

            // Metallic + roughness
            float metallic = 0.0f, roughness = 1.0f;
            aiGetMaterialFloat(aiMat, AI_MATKEY_METALLIC_FACTOR, &metallic);
            aiGetMaterialFloat(aiMat, AI_MATKEY_ROUGHNESS_FACTOR, &roughness);
            material.setMetalness(metallic);
            material.setMetallicFactor(metallic);
            material.setGloss(1.0f - roughness);
            material.setRoughnessFactor(roughness);
            material.setUseMetalness(true);

            // Emissive
            aiColor4D emissive(0.0f, 0.0f, 0.0f, 1.0f);
            aiGetMaterialColor(aiMat, AI_MATKEY_COLOR_EMISSIVE, &emissive);
            if (emissive.r > 0.0f || emissive.g > 0.0f || emissive.b > 0.0f) {
                material.setEmissive(Color(emissive.r, emissive.g, emissive.b, 1.0f));
                material.setEmissiveFactor(Color(emissive.r, emissive.g, emissive.b, 1.0f));
            }

            // Opacity
            float opacity = 1.0f;
            aiGetMaterialFloat(aiMat, AI_MATKEY_OPACITY, &opacity);
            material.setOpacity(opacity);
            if (opacity < 0.99f) {
                material.setAlphaMode(AlphaMode::BLEND);
                material.setTransparent(true);
            }

            // Textures
            if (auto* tex = loadAndOwn(aiTextureType_BASE_COLOR)) {
                material.setDiffuseMap(tex);
                material.setBaseColorTexture(tex);
                material.setHasBaseColorTexture(true);
            } else if (auto* tex2 = loadAndOwn(aiTextureType_DIFFUSE)) {
                material.setDiffuseMap(tex2);
                material.setBaseColorTexture(tex2);
                material.setHasBaseColorTexture(true);
            }

            if (auto* tex = loadAndOwn(aiTextureType_NORMALS)) {
                material.setNormalMap(tex);
                material.setNormalTexture(tex);
                material.setHasNormalTexture(true);
            }

            // Metallic-roughness combined texture (glTF aiTextureType_UNKNOWN)
            if (auto* tex = loadAndOwn(aiTextureType_UNKNOWN)) {
                material.setMetallicRoughnessTexture(tex);
                material.setHasMetallicRoughnessTexture(true);
            }

            // AO (glTF uses aiTextureType_LIGHTMAP)
            if (auto* tex = loadAndOwn(aiTextureType_LIGHTMAP)) {
                material.setAoMap(tex);
                material.setOcclusionTexture(tex);
                material.setHasOcclusionTexture(true);
            }

            // Emissive map
            if (auto* tex = loadAndOwn(aiTextureType_EMISSIVE)) {
                material.setEmissiveMap(tex);
                material.setEmissiveTexture(tex);
                material.setHasEmissiveTexture(true);
            }
        }

        // ── Legacy material conversion (Phong/Lambert/Blinn → PBR) ─────

        void convertLegacyMaterial(
            const aiMaterial* aiMat,
            const aiScene* scene,
            const std::filesystem::path& basedir,
            GraphicsDevice* device,
            StandardMaterial& material,
            std::unordered_map<std::string, std::shared_ptr<Texture>>& texCache,
            std::vector<std::shared_ptr<Texture>>& ownedTextures)
        {
            auto loadAndOwn = [&](aiTextureType type) -> Texture* {
                auto tex = loadAssimpTexture(aiMat, type, scene, basedir, device, texCache);
                if (tex) ownedTextures.push_back(tex);
                return tex.get();
            };

            // Diffuse -> Base Color
            aiColor4D diffuse(0.8f, 0.8f, 0.8f, 1.0f);
            aiGetMaterialColor(aiMat, AI_MATKEY_COLOR_DIFFUSE, &diffuse);
            material.setDiffuse(Color(diffuse.r, diffuse.g, diffuse.b, diffuse.a));
            material.setBaseColorFactor(Color(diffuse.r, diffuse.g, diffuse.b, diffuse.a));

            // Specular
            aiColor4D specular(0.0f, 0.0f, 0.0f, 1.0f);
            aiGetMaterialColor(aiMat, AI_MATKEY_COLOR_SPECULAR, &specular);
            material.setSpecular(Color(specular.r, specular.g, specular.b, 1.0f));

            // Shininess -> Roughness (physically-motivated formula)
            float shininess = 0.0f;
            aiGetMaterialFloat(aiMat, AI_MATKEY_SHININESS, &shininess);
            float roughness = (shininess > 0.0f)
                ? std::sqrt(2.0f / (shininess + 2.0f))
                : 1.0f;
            material.setGloss(1.0f - roughness);
            material.setRoughnessFactor(roughness);

            // Metalness heuristic (same algorithm as ObjParser)
            float kdLum = 0.2126f * diffuse.r + 0.7152f * diffuse.g + 0.0722f * diffuse.b;
            float ksLum = 0.2126f * specular.r + 0.7152f * specular.g + 0.0722f * specular.b;
            float metalness = 0.0f;
            if (kdLum < 0.04f && ksLum > 0.5f) {
                metalness = 1.0f;
            } else if (ksLum > 0.25f) {
                float ksMax = std::max({specular.r, specular.g, specular.b});
                float ksMin = std::min({specular.r, specular.g, specular.b});
                float sat = (ksMax > 0.001f) ? (ksMax - ksMin) / ksMax : 0.0f;
                if (sat > 0.2f) metalness = 0.8f;
            }
            material.setMetalness(metalness);
            material.setMetallicFactor(metalness);
            material.setUseMetalness(true);

            // Emissive
            aiColor4D emissive(0.0f, 0.0f, 0.0f, 1.0f);
            aiGetMaterialColor(aiMat, AI_MATKEY_COLOR_EMISSIVE, &emissive);
            if (emissive.r > 0.0f || emissive.g > 0.0f || emissive.b > 0.0f) {
                material.setEmissive(Color(emissive.r, emissive.g, emissive.b, 1.0f));
                material.setEmissiveFactor(Color(emissive.r, emissive.g, emissive.b, 1.0f));
            }

            // Opacity
            float opacity = 1.0f;
            aiGetMaterialFloat(aiMat, AI_MATKEY_OPACITY, &opacity);
            material.setOpacity(opacity);
            if (opacity < 0.99f) {
                material.setAlphaMode(AlphaMode::BLEND);
                material.setTransparent(true);
            }

            material.setCullMode(CullMode::CULLFACE_BACK);

            // Textures: diffuse / base color map
            if (auto* tex = loadAndOwn(aiTextureType_DIFFUSE)) {
                material.setDiffuseMap(tex);
                material.setBaseColorTexture(tex);
                material.setHasBaseColorTexture(true);
            }

            // Normal map: check NORMALS first, fall back to HEIGHT (bump maps)
            if (auto* tex = loadAndOwn(aiTextureType_NORMALS)) {
                material.setNormalMap(tex);
                material.setNormalTexture(tex);
                material.setHasNormalTexture(true);
            } else if (auto* tex2 = loadAndOwn(aiTextureType_HEIGHT)) {
                material.setNormalMap(tex2);
                material.setNormalTexture(tex2);
                material.setHasNormalTexture(true);
                material.setBumpiness(0.5f);
                material.setNormalScale(0.5f);
            }

            // AO (Collada ambient maps)
            if (auto* tex = loadAndOwn(aiTextureType_AMBIENT)) {
                material.setAoMap(tex);
                material.setOcclusionTexture(tex);
                material.setHasOcclusionTexture(true);
            }

            // Emissive map
            if (auto* tex = loadAndOwn(aiTextureType_EMISSIVE)) {
                material.setEmissiveMap(tex);
                material.setEmissiveTexture(tex);
                material.setHasEmissiveTexture(true);
            }

            // Opacity map
            if (auto* tex = loadAndOwn(aiTextureType_OPACITY)) {
                material.setOpacityMap(tex);
                material.setAlphaMode(AlphaMode::MASK);
            }
        }

        // ── Shader variant key (same bit assignments as GlbParser) ─────

        uint64_t computeShaderVariantKey(const Material& material)
        {
            uint64_t variant = 1;  // base bit always set
            if (material.hasBaseColorTexture())         variant |= (1ull << 1);
            if (material.alphaMode() == AlphaMode::BLEND) variant |= (1ull << 2);
            else if (material.alphaMode() == AlphaMode::MASK) variant |= (1ull << 3);
            if (material.hasNormalTexture())             variant |= (1ull << 4);
            if (material.hasMetallicRoughnessTexture())  variant |= (1ull << 5);
            if (material.hasOcclusionTexture())          variant |= (1ull << 6);
            if (material.hasEmissiveTexture())           variant |= (1ull << 7);
            return variant;
        }

        // ── Mesh conversion (aiMesh → Mesh + VertexBuffer + IndexBuffer) ──

        std::shared_ptr<Mesh> convertAssimpMesh(
            const aiMesh* aiM,
            const std::shared_ptr<VertexFormat>& vertexFormat,
            const std::shared_ptr<GraphicsDevice>& device,
            const AssimpParserConfig& config)
        {
            const unsigned int vertexCount = aiM->mNumVertices;
            if (vertexCount == 0) return nullptr;

            std::vector<PackedVertex> vertices(vertexCount);

            float minX = std::numeric_limits<float>::max();
            float minY = std::numeric_limits<float>::max();
            float minZ = std::numeric_limits<float>::max();
            float maxX = std::numeric_limits<float>::lowest();
            float maxY = std::numeric_limits<float>::lowest();
            float maxZ = std::numeric_limits<float>::lowest();

            const bool hasNormals  = aiM->HasNormals();
            const bool hasUVs      = aiM->HasTextureCoords(0);
            const bool hasUV1      = aiM->HasTextureCoords(1);
            const bool hasTangents = aiM->HasTangentsAndBitangents();

            for (unsigned int i = 0; i < vertexCount; ++i) {
                float px = aiM->mVertices[i].x;
                float py = aiM->mVertices[i].y;
                float pz = aiM->mVertices[i].z;

                // Apply config transforms
                px *= config.uniformScale;
                py *= config.uniformScale;
                pz *= config.uniformScale;
                if (config.flipYZ) {
                    std::swap(py, pz);
                    pz = -pz;
                }

                float nx = 0.0f, ny = 1.0f, nz = 0.0f;
                if (hasNormals) {
                    nx = aiM->mNormals[i].x;
                    ny = aiM->mNormals[i].y;
                    nz = aiM->mNormals[i].z;
                    if (config.flipYZ) {
                        std::swap(ny, nz);
                        nz = -nz;
                    }
                }

                // UVs already flipped by aiProcess_FlipUVs -- no manual flip needed
                float u = 0.0f, v = 0.0f;
                if (hasUVs) {
                    u = aiM->mTextureCoords[0][i].x;
                    v = aiM->mTextureCoords[0][i].y;
                }

                float u1 = u, v1 = v;
                if (hasUV1) {
                    u1 = aiM->mTextureCoords[1][i].x;
                    v1 = aiM->mTextureCoords[1][i].y;
                }

                float tx = 0.0f, ty = 0.0f, tz = 0.0f, tw = 1.0f;
                if (hasTangents) {
                    tx = aiM->mTangents[i].x;
                    ty = aiM->mTangents[i].y;
                    tz = aiM->mTangents[i].z;
                    if (config.flipYZ) {
                        std::swap(ty, tz);
                        tz = -tz;
                    }
                    // Compute handedness from bitangent
                    Vector3 n(nx, ny, nz);
                    Vector3 t(tx, ty, tz);
                    float bx = aiM->mBitangents[i].x;
                    float by = aiM->mBitangents[i].y;
                    float bz = aiM->mBitangents[i].z;
                    if (config.flipYZ) {
                        std::swap(by, bz);
                        bz = -bz;
                    }
                    Vector3 b(bx, by, bz);
                    tw = (n.cross(t).dot(b) < 0.0f) ? -1.0f : 1.0f;
                } else {
                    tangentFromNormal(nx, ny, nz, tx, ty, tz, tw);
                }

                vertices[i] = PackedVertex{
                    px, py, pz,
                    nx, ny, nz,
                    u, v,
                    tx, ty, tz, tw,
                    u1, v1
                };

                minX = std::min(minX, px); minY = std::min(minY, py); minZ = std::min(minZ, pz);
                maxX = std::max(maxX, px); maxY = std::max(maxY, py); maxZ = std::max(maxZ, pz);
            }

            // Build index buffer
            std::vector<uint32_t> indices;
            indices.reserve(static_cast<size_t>(aiM->mNumFaces) * 3);
            for (unsigned int f = 0; f < aiM->mNumFaces; ++f) {
                const aiFace& face = aiM->mFaces[f];
                if (face.mNumIndices == 3) {
                    if (config.flipWinding) {
                        indices.push_back(face.mIndices[0]);
                        indices.push_back(face.mIndices[2]);
                        indices.push_back(face.mIndices[1]);
                    } else {
                        indices.push_back(face.mIndices[0]);
                        indices.push_back(face.mIndices[1]);
                        indices.push_back(face.mIndices[2]);
                    }
                }
            }

            if (indices.empty()) return nullptr;

            // Generate tangents via Lengyel if Assimp didn't provide them and UVs exist
            if (!hasTangents && hasUVs && config.generateTangents) {
                generateTangents(vertices, indices);
            }

            // Create GPU vertex buffer
            const int vCount = static_cast<int>(vertices.size());
            std::vector<uint8_t> vertexBytes(vertices.size() * sizeof(PackedVertex));
            std::memcpy(vertexBytes.data(), vertices.data(), vertexBytes.size());

            VertexBufferOptions vbOpts;
            vbOpts.usage = BUFFER_STATIC;
            vbOpts.data = std::move(vertexBytes);
            auto vb = device->createVertexBuffer(vertexFormat, vCount, vbOpts);

            // Create GPU index buffer (uint16 for small, uint32 for large)
            const int indexCount = static_cast<int>(indices.size());
            IndexFormat idxFmt = (vCount <= 65535) ? INDEXFORMAT_UINT16 : INDEXFORMAT_UINT32;

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

            // Create Mesh
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

            // Use Assimp's pre-computed AABB if valid, otherwise use our computed bounds
            if (aiM->mAABB.mMin.x <= aiM->mAABB.mMax.x &&
                aiM->mAABB.mMin.y <= aiM->mAABB.mMax.y &&
                aiM->mAABB.mMin.z <= aiM->mAABB.mMax.z &&
                config.uniformScale == 1.0f && !config.flipYZ)
            {
                minX = aiM->mAABB.mMin.x; minY = aiM->mAABB.mMin.y; minZ = aiM->mAABB.mMin.z;
                maxX = aiM->mAABB.mMax.x; maxY = aiM->mAABB.mMax.y; maxZ = aiM->mAABB.mMax.z;
            }

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

            return meshResource;
        }

    } // anonymous namespace

    // ── AssimpParser::parse ─────────────────────────────────────────────

    std::unique_ptr<GlbContainerResource> AssimpParser::parse(
        const std::string& path,
        const std::shared_ptr<GraphicsDevice>& device,
        const AssimpParserConfig& config)
    {
        if (!device) {
            spdlog::error("Assimp parse failed: graphics device is null");
            return nullptr;
        }

        // ── Configure importer ─────────────────────────────────────────

        Assimp::Importer importer;
        importer.SetPropertyFloat(AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE, config.smoothingAngle);
        importer.SetPropertyFloat(AI_CONFIG_PP_CT_MAX_SMOOTHING_ANGLE, 45.0f);

        if (config.uniformScale != 1.0f) {
            importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, config.uniformScale);
        }

        // ── Build post-processing flags ────────────────────────────────

        unsigned int processFlags =
            aiProcess_Triangulate |
            aiProcess_GenSmoothNormals |
            aiProcess_JoinIdenticalVertices |
            aiProcess_FlipUVs |
            aiProcess_ImproveCacheLocality |
            aiProcess_RemoveRedundantMaterials |
            aiProcess_FindDegenerates |
            aiProcess_FindInvalidData |
            aiProcess_GenUVCoords |
            aiProcess_SortByPType |
            aiProcess_GenBoundingBoxes;

        if (config.generateTangents) {
            processFlags |= aiProcess_CalcTangentSpace;
        }
        if (config.optimizeMeshes) {
            processFlags |= aiProcess_OptimizeMeshes;
        }
        if (config.uniformScale != 1.0f) {
            processFlags |= aiProcess_GlobalScale;
        }

        // ── Import ─────────────────────────────────────────────────────

        const aiScene* scene = importer.ReadFile(path, processFlags);
        if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
            spdlog::error("Assimp parse failed [{}]: {}", path, importer.GetErrorString());
            return nullptr;
        }

        const std::filesystem::path modelPath(path);
        const std::filesystem::path basedir = modelPath.parent_path();

        auto container = std::make_unique<GlbContainerResource>();
        auto vertexFormat = std::make_shared<VertexFormat>(
            static_cast<int>(sizeof(PackedVertex)), true, false);

        // ── Convert materials ──────────────────────────────────────────

        std::unordered_map<std::string, std::shared_ptr<Texture>> texCache;
        std::vector<std::shared_ptr<Texture>> ownedTextures;
        std::vector<std::shared_ptr<Material>> materials;

        for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
            const aiMaterial* aiMat = scene->mMaterials[i];
            auto material = std::make_shared<StandardMaterial>();

            // Name
            aiString matName;
            if (aiGetMaterialString(aiMat, AI_MATKEY_NAME, &matName) == AI_SUCCESS
                && matName.length > 0) {
                material->setName(matName.C_Str());
            } else {
                material->setName("assimp-material-" + std::to_string(i));
            }

            // Detect shading model
            int shadingModel = 0;
            aiGetMaterialInteger(aiMat, AI_MATKEY_SHADING_MODEL, &shadingModel);

            if (shadingModel == aiShadingMode_PBR_BRDF) {
                convertPbrMaterial(aiMat, scene, basedir, device.get(),
                                   *material, texCache, ownedTextures);
            } else {
                convertLegacyMaterial(aiMat, scene, basedir, device.get(),
                                      *material, texCache, ownedTextures);
            }

            // Double-sided
            int twoSided = 0;
            if (aiGetMaterialInteger(aiMat, AI_MATKEY_TWOSIDED, &twoSided) == AI_SUCCESS && twoSided) {
                material->setCullMode(CullMode::CULLFACE_NONE);
            }

            // Shader variant key
            material->setShaderVariantKey(computeShaderVariantKey(*material));

            materials.push_back(material);
        }

        // Default material if scene has no materials
        if (materials.empty()) {
            auto mat = std::make_shared<StandardMaterial>();
            mat->setName("assimp-default");
            mat->setDiffuse(Color(0.8f, 0.8f, 0.8f, 1.0f));
            mat->setBaseColorFactor(Color(0.8f, 0.8f, 0.8f, 1.0f));
            mat->setMetalness(0.0f);
            mat->setMetallicFactor(0.0f);
            mat->setGloss(0.5f);
            mat->setRoughnessFactor(0.5f);
            mat->setUseMetalness(true);
            mat->setShaderVariantKey(1);
            materials.push_back(mat);
        }

        // ── Convert meshes ─────────────────────────────────────────────

        // Map from aiScene mesh index to GlbMeshPayload index
        std::vector<size_t> meshToPayloadIndex(scene->mNumMeshes, SIZE_MAX);
        size_t nextPayloadIndex = 0;

        for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
            const aiMesh* aiM = scene->mMeshes[i];

            // Skip non-triangle meshes (points/lines from aiProcess_SortByPType)
            if (aiM->mPrimitiveTypes != aiPrimitiveType_TRIANGLE)
                continue;

            auto meshResource = convertAssimpMesh(aiM, vertexFormat, device, config);
            if (!meshResource)
                continue;

            GlbMeshPayload payload;
            payload.mesh = meshResource;
            unsigned int matIdx = aiM->mMaterialIndex;
            payload.material = (matIdx < materials.size())
                ? materials[matIdx]
                : materials[0];
            container->addMeshPayload(payload);
            meshToPayloadIndex[i] = nextPayloadIndex++;
        }

        // ── Build node hierarchy ───────────────────────────────────────

        // Pre-order DFS to collect all nodes into a flat array
        std::vector<const aiNode*> allNodes;
        std::unordered_map<const aiNode*, int> nodeIndexMap;

        std::function<void(const aiNode*)> collectNodes = [&](const aiNode* node) {
            int idx = static_cast<int>(allNodes.size());
            allNodes.push_back(node);
            nodeIndexMap[node] = idx;
            for (unsigned int c = 0; c < node->mNumChildren; ++c) {
                collectNodes(node->mChildren[c]);
            }
        };
        collectNodes(scene->mRootNode);

        // Build GlbNodePayloads
        for (size_t i = 0; i < allNodes.size(); ++i) {
            const aiNode* node = allNodes[i];
            GlbNodePayload nodePayload;
            nodePayload.name = node->mName.C_Str();

            // Decompose local transform
            aiVector3D scaling, position;
            aiQuaternion rotation;
            node->mTransformation.Decompose(scaling, rotation, position);

            nodePayload.translation = Vector3(position.x, position.y, position.z);
            nodePayload.rotation = Quaternion(rotation.x, rotation.y, rotation.z, rotation.w);
            nodePayload.scale = Vector3(scaling.x, scaling.y, scaling.z);

            // Map node's meshes to payload indices
            for (unsigned int m = 0; m < node->mNumMeshes; ++m) {
                unsigned int meshIdx = node->mMeshes[m];
                if (meshIdx < meshToPayloadIndex.size() && meshToPayloadIndex[meshIdx] != SIZE_MAX) {
                    nodePayload.meshPayloadIndices.push_back(meshToPayloadIndex[meshIdx]);
                }
            }

            // Set children indices
            for (unsigned int c = 0; c < node->mNumChildren; ++c) {
                auto childIt = nodeIndexMap.find(node->mChildren[c]);
                if (childIt != nodeIndexMap.end()) {
                    nodePayload.children.push_back(childIt->second);
                }
            }

            container->addNodePayload(nodePayload);
        }

        // Root node is index 0
        container->addRootNodeIndex(0);

        // Transfer texture ownership
        for (auto& tex : ownedTextures) {
            container->addOwnedTexture(tex);
        }

        spdlog::info("Assimp parse complete [{}]: {} meshes, {} materials, {} nodes, {} textures",
            path, nextPayloadIndex, materials.size(), allNodes.size(), ownedTextures.size());

        return container;
    }

} // namespace visutwin::canvas
