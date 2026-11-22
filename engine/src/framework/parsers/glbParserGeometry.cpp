// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// glTF parser: primitives (triangles, Draco, point clouds, gaussian splats) and their meshes.
#include "glbParserInternal.h"

#include "glbParser.h"
#include "framework/parsers/packedVertex.h"

#include "framework/components/light/lightComponent.h"

#include <algorithm>
#include <atomic>
#include <numbers>
#include <mutex>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <draco/compression/decode.h>
#include <draco/core/decoder_buffer.h>
#include <draco/mesh/mesh.h>

#include "core/shape/boundingBox.h"
#include "core/math/matrix4.h"
#include "core/math/vector4.h"
#include "core/math/vector3.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/textureMips.h"
#include "platform/graphics/textureUtils.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/gsplat/gsplatResource.h"
#include "scene/materials/standardMaterial.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    using namespace gltf;

    namespace
    {
        bool primitiveUsesDraco(const tinygltf::Primitive& primitive)
        {
            return primitive.extensions.contains("KHR_draco_mesh_compression");
        }

        int readDracoAttributeId(const tinygltf::Value& dracoExtension, const std::string& semantic)
        {
            if (!dracoExtension.IsObject() || !dracoExtension.Has("attributes")) {
                return -1;
            }
            const auto attrs = dracoExtension.Get("attributes");
            if (!attrs.IsObject() || !attrs.Has(semantic)) {
                return -1;
            }
            const auto& idValue = attrs.Get(semantic);
            if (idValue.IsInt()) {
                return idValue.Get<int>();
            }
            if (idValue.IsNumber()) {
                return idValue.GetNumberAsInt();
            }
            return -1;
        }

        const draco::PointAttribute* getDracoAttribute(const draco::Mesh& mesh, const tinygltf::Value& dracoExtension,
            const std::string& semantic)
        {
            const int uniqueId = readDracoAttributeId(dracoExtension, semantic);
            if (uniqueId < 0) {
                return nullptr;
            }
            return mesh.GetAttributeByUniqueId(uniqueId);
        }

        // Decodes a KHR_draco_mesh_compression primitive into vertices and a triangle
        // list. `outHasNormals` / `outHasTangents` say whether the payload carried those
        // attributes (the caller derives what is missing), and `outColors` receives
        // COLOR_0 as RGBA per vertex, or stays empty.
        bool decodeDracoPrimitive(const tinygltf::Model& model, const tinygltf::Primitive& primitive,
            std::vector<PackedVertex>& outVertices, std::vector<uint32_t>& outIndices, Vector3& outMinPos, Vector3& outMaxPos,
            bool& outHasNormals, bool& outHasTangents, std::vector<float>& outColors)
        {
            const auto extIt = primitive.extensions.find("KHR_draco_mesh_compression");
            if (extIt == primitive.extensions.end()) {
                return false;
            }
            const auto& dracoExt = extIt->second;
            if (!dracoExt.IsObject() || !dracoExt.Has("bufferView")) {
                spdlog::warn("glTF primitive has malformed KHR_draco_mesh_compression extension");
                return false;
            }

            const auto bufferViewVal = dracoExt.Get("bufferView");
            const int bufferViewIndex = bufferViewVal.IsInt() ? bufferViewVal.Get<int>() :
                (bufferViewVal.IsNumber() ? bufferViewVal.GetNumberAsInt() : -1);
            if (bufferViewIndex < 0 || bufferViewIndex >= static_cast<int>(model.bufferViews.size())) {
                spdlog::warn("Draco primitive references invalid bufferView {}", bufferViewIndex);
                return false;
            }

            const auto& view = model.bufferViews[static_cast<size_t>(bufferViewIndex)];
            if (view.buffer < 0 || view.buffer >= static_cast<int>(model.buffers.size())) {
                spdlog::warn("Draco primitive bufferView references invalid buffer {}", view.buffer);
                return false;
            }

            const auto& buffer = model.buffers[static_cast<size_t>(view.buffer)];
            const size_t byteOffset = static_cast<size_t>(view.byteOffset);
            const size_t byteLength = static_cast<size_t>(view.byteLength);
            if (byteOffset + byteLength > buffer.data.size()) {
                spdlog::warn("Draco primitive compressed payload out of bounds");
                return false;
            }

            draco::DecoderBuffer decoderBuffer;
            decoderBuffer.Init(reinterpret_cast<const char*>(buffer.data.data() + byteOffset),
                static_cast<int64_t>(byteLength));

            draco::Decoder decoder;
            auto meshStatus = decoder.DecodeMeshFromBuffer(&decoderBuffer);
            if (!meshStatus.ok()) {
                spdlog::warn("Failed to decode Draco mesh: {}", meshStatus.status().error_msg_string());
                return false;
            }

            std::unique_ptr<draco::Mesh> dracoMesh = std::move(meshStatus).value();
            if (!dracoMesh || dracoMesh->num_points() <= 0) {
                spdlog::warn("Decoded Draco mesh has no points");
                return false;
            }

            const auto* positionAttr = getDracoAttribute(*dracoMesh, dracoExt, "POSITION");
            if (!positionAttr || positionAttr->num_components() < 3) {
                spdlog::warn("Decoded Draco mesh missing POSITION attribute");
                return false;
            }
            const auto* normalAttr = getDracoAttribute(*dracoMesh, dracoExt, "NORMAL");
            const auto* uvAttr = getDracoAttribute(*dracoMesh, dracoExt, "TEXCOORD_0");
            const auto* uv1Attr = getDracoAttribute(*dracoMesh, dracoExt, "TEXCOORD_1");
            const auto* tangentAttr = getDracoAttribute(*dracoMesh, dracoExt, "TANGENT");
            const auto* colorAttr = getDracoAttribute(*dracoMesh, dracoExt, "COLOR_0");
            outHasNormals = normalAttr && normalAttr->num_components() >= 3;
            outHasTangents = tangentAttr && tangentAttr->num_components() >= 4;
            const bool hasColors = colorAttr && colorAttr->num_components() >= 3;

            const int32_t pointCount = dracoMesh->num_points();
            outVertices.resize(static_cast<size_t>(pointCount));
            outColors.clear();
            if (hasColors) {
                outColors.assign(static_cast<size_t>(pointCount) * 4, 1.0f);
            }

            outMinPos = Vector3(std::numeric_limits<float>::max());
            outMaxPos = Vector3(std::numeric_limits<float>::lowest());

            for (int32_t i = 0; i < pointCount; ++i) {
                const draco::PointIndex pointIndex(i);
                const draco::AttributeValueIndex positionValueIndex = positionAttr->mapped_index(pointIndex);
                if (positionValueIndex < 0) {
                    spdlog::warn("Decoded Draco POSITION has invalid mapped index at point {}", i);
                    return false;
                }

                std::array<float, 3> pos{0.0f, 0.0f, 0.0f};
                if (!positionAttr->ConvertValue<float, 3>(positionValueIndex, pos.data())) {
                    spdlog::warn("Failed to decode Draco POSITION at vertex {}", i);
                    return false;
                }

                // Overwritten by generated face normals when the payload has none.
                std::array<float, 3> normal{0.0f, 1.0f, 0.0f};
                if (outHasNormals) {
                    const draco::AttributeValueIndex normalValueIndex = normalAttr->mapped_index(pointIndex);
                    if (normalValueIndex >= 0) {
                        normalAttr->ConvertValue<float, 3>(normalValueIndex, normal.data());
                    }
                }

                std::array<float, 2> uv{0.0f, 0.0f};
                if (uvAttr && uvAttr->num_components() >= 2) {
                    const draco::AttributeValueIndex uvValueIndex = uvAttr->mapped_index(pointIndex);
                    if (uvValueIndex >= 0) {
                        uvAttr->ConvertValue<float, 2>(uvValueIndex, uv.data());
                    }
                }

                std::array<float, 2> uv1{uv[0], uv[1]};
                if (uv1Attr && uv1Attr->num_components() >= 2) {
                    const draco::AttributeValueIndex uv1ValueIndex = uv1Attr->mapped_index(pointIndex);
                    if (uv1ValueIndex >= 0) {
                        uv1Attr->ConvertValue<float, 2>(uv1ValueIndex, uv1.data());
                    }
                }

                std::array<float, 4> tangent{0.0f, 0.0f, 0.0f, 1.0f};
                if (outHasTangents) {
                    const draco::AttributeValueIndex tangentValueIndex = tangentAttr->mapped_index(pointIndex);
                    if (tangentValueIndex >= 0) {
                        // Taken as the file has it: glTF's bitangent, cross(N, T) * w, points
                        // toward the image's top row, as the engine's does.
                        tangentAttr->ConvertValue<float, 4>(tangentValueIndex, tangent.data());
                    }
                }

                outVertices[static_cast<size_t>(i)] = PackedVertex{
                    pos[0], pos[1], pos[2],
                    normal[0], normal[1], normal[2],
                    uv[0], uv[1],
                    tangent[0], tangent[1], tangent[2], tangent[3],
                    uv1[0], uv1[1]
                };

                if (hasColors) {
                    const draco::AttributeValueIndex colorValueIndex = colorAttr->mapped_index(pointIndex);
                    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
                    if (colorValueIndex >= 0) {
                        colorAttr->ConvertValue<float, 4>(colorValueIndex, color.data());
                    }
                    // A three-component colour comes back with a zero fourth component.
                    std::copy_n(color.data(), 3, outColors.data() + static_cast<size_t>(i) * 4);
                }

                const Vector3 position = Vector3::load(pos.data());
                outMinPos = Vector3::min(outMinPos, position);
                outMaxPos = Vector3::max(outMaxPos, position);
            }

            outIndices.clear();
            outIndices.reserve(static_cast<size_t>(dracoMesh->num_faces()) * 3);
            for (draco::FaceIndex faceIndex(0); faceIndex < dracoMesh->num_faces(); ++faceIndex) {
                const auto& face = dracoMesh->face(faceIndex);
                outIndices.push_back(face[0].value());
                outIndices.push_back(face[1].value());
                outIndices.push_back(face[2].value());
            }
            return true;
        }

        PrimitiveType mapPrimitiveType(const int mode)
        {
            switch (mode) {
            case TINYGLTF_MODE_POINTS:
                return PRIMITIVE_POINTS;
            case TINYGLTF_MODE_LINE:
                return PRIMITIVE_LINES;
            case TINYGLTF_MODE_LINE_LOOP:
                return PRIMITIVE_LINELOOP;
            case TINYGLTF_MODE_LINE_STRIP:
                return PRIMITIVE_LINESTRIP;
            case TINYGLTF_MODE_TRIANGLE_STRIP:
                return PRIMITIVE_TRISTRIP;
            case TINYGLTF_MODE_TRIANGLE_FAN:
                return PRIMITIVE_TRIFAN;
            case TINYGLTF_MODE_TRIANGLES:
            default:
                return PRIMITIVE_TRIANGLES;
            }
        }
    }

    namespace gltf
    {
        bool isGaussianSplatPrimitive(const tinygltf::Primitive& primitive)
        {
            return primitive.extensions.contains("KHR_gaussian_splatting");
        }

        // KHR_gaussian_splatting: the splat attributes of a
        // POINTS primitive, read through AccessorReader so every component type the file
        // may use is de-quantised. The values are ACTIVATED — linear scale, post-sigmoid
        // opacity. Null, with an error, when a required attribute is missing or its
        // count does not match POSITION's. SH bands count only while complete.
        // The extension's sortingMethod and projection are
        // ignored; an unsupported kernel or a linear colour
        // space is warned about and rendered as the default.
        std::unique_ptr<GSplatData> gaussianSplatData(const tinygltf::Model& model,
            const tinygltf::Primitive& primitive, const std::string& source)
        {
            static constexpr const char* kExt = "KHR_gaussian_splatting";
            const auto& ext = primitive.extensions.at(kExt);
            if (ext.IsObject() && ext.Has("kernel") && ext.Get("kernel").IsString() &&
                ext.Get("kernel").Get<std::string>() != "ellipse") {
                spdlog::warn("GLB [{}]: {} kernel '{}' is not supported, rendering as 'ellipse'", source, kExt,
                    ext.Get("kernel").Get<std::string>());
            }
            if (ext.IsObject() && ext.Has("colorSpace") && ext.Get("colorSpace").IsString() &&
                ext.Get("colorSpace").Get<std::string>() == "lin_rec709_display") {
                spdlog::warn("GLB [{}]: {} colour space 'lin_rec709_display' is not supported, "
                    "treated as srgb_rec709_display", source, kExt);
            }

            const auto* positions = primitiveAttribute(model, primitive, "POSITION");
            const size_t count = positions ? positions->count : 0;
            const auto read = [&](const std::string& name, const int type, const int components,
                                  std::vector<float>& out) -> bool {
                const auto* accessor = primitiveAttribute(model, primitive, name.c_str());
                if (!accessor || accessor->count != count || accessor->type != type) {
                    return false;
                }
                const AccessorReader reader(model, *accessor, type);
                out.resize(count * static_cast<size_t>(components));
                for (size_t i = 0; i < count; ++i) {
                    if (!reader.read(i, out.data() + i * static_cast<size_t>(components), components)) {
                        return false;
                    }
                }
                return true;
            };

            GSplatData::ActivatedSplats splats;
            splats.count = count;
            const std::string prefix = std::string(kExt) + ":";
            if (count == 0 || !read("POSITION", TINYGLTF_TYPE_VEC3, 3, splats.positions) ||
                !read(prefix + "ROTATION", TINYGLTF_TYPE_VEC4, 4, splats.rotations) ||
                !read(prefix + "SCALE", TINYGLTF_TYPE_VEC3, 3, splats.scales) ||
                !read(prefix + "OPACITY", TINYGLTF_TYPE_SCALAR, 1, splats.opacities) ||
                !read(prefix + "SH_DEGREE_0_COEF_0", TINYGLTF_TYPE_VEC3, 3, splats.sh0)) {
                spdlog::error("GLB [{}]: a {} primitive is missing required attributes or their data is invalid; "
                    "the primitive is skipped", source, kExt);
                return nullptr;
            }

            // Higher bands: degree d has 2d + 1 coefficients, each an RGB attribute.
            // Gathered coefficient-major, the layout the splat data keeps.
            static constexpr int kDegreeCoeffs[] = {3, 5, 7};
            int bands = 0;
            for (int d = 1; d <= 3; ++d) {
                bool complete = true;
                for (int c = 0; c < kDegreeCoeffs[d - 1]; ++c) {
                    if (!primitive.attributes.contains(prefix + "SH_DEGREE_" + std::to_string(d) + "_COEF_" + std::to_string(c))) {
                        complete = false;
                        break;
                    }
                }
                if (!complete) {
                    break;
                }
                bands = d;
            }
            if (bands > 0) {
                static constexpr int kBandCoeffs[] = {0, 3, 8, 15};
                const int coeffs = kBandCoeffs[bands];
                splats.shRest.assign(count * static_cast<size_t>(coeffs) * 3, 0.0f);
                int k = 0;
                std::vector<float> coefficient;
                for (int d = 1; d <= bands; ++d) {
                    for (int c = 0; c < kDegreeCoeffs[d - 1]; ++c, ++k) {
                        const std::string name = prefix + "SH_DEGREE_" + std::to_string(d) + "_COEF_" + std::to_string(c);
                        if (!read(name, TINYGLTF_TYPE_VEC3, 3, coefficient)) {
                            spdlog::error("GLB [{}]: a {} primitive has an invalid {} attribute; the primitive is skipped",
                                source, kExt, name);
                            return nullptr;
                        }
                        for (size_t i = 0; i < count; ++i) {
                            float* dst = splats.shRest.data() + (i * static_cast<size_t>(coeffs) + static_cast<size_t>(k)) * 3;
                            dst[0] = coefficient[i * 3];
                            dst[1] = coefficient[i * 3 + 1];
                            dst[2] = coefficient[i * 3 + 2];
                        }
                    }
                }
                splats.shBands = bands;
            }
            return GSplatData::fromActivated(splats, source);
        }

        // Positions and COLOR_0 of a POINTS primitive, transformed by `transform` when
        // one is given (the static merge bakes world space; an animated model keeps
        // local space so the node's animation still moves the cloud).
        void appendPointVertices(const tinygltf::Model& model, const tinygltf::Accessor& positions,
            const tinygltf::Accessor* colors, const Matrix4* transform,
            std::vector<PackedPointVertex>& out, Vector3& boundsMin, Vector3& boundsMax)
        {
            const size_t base = out.size();
            const auto count = static_cast<size_t>(positions.count);
            out.resize(base + count);
            const AccessorReader positionReader(model, positions, TINYGLTF_TYPE_VEC3);
            // COLOR_0 is VEC3 or VEC4; a VEC3 colour keeps alpha 1.
            const AccessorReader colorReader = colors &&
                (colors->type == TINYGLTF_TYPE_VEC3 || colors->type == TINYGLTF_TYPE_VEC4)
                ? AccessorReader(model, *colors) : AccessorReader();
            const int colorComponents = colorReader.valid() ? colorReader.components() : 0;
            for (size_t i = 0; i < count; ++i) {
                float p[3] = {0.0f, 0.0f, 0.0f};
                if (!positionReader.read(i, p, 3)) {
                    continue;
                }
                Vector3 pos = Vector3::load(p);
                if (transform) {
                    pos = transform->transformPoint(pos);
                }
                float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                if (colorComponents > 0) {
                    colorReader.read(i, color, colorComponents);
                }
                out[base + i] = PackedPointVertex{pos.getX(), pos.getY(), pos.getZ(),
                    color[0], color[1], color[2], color[3]};
                boundsMin = Vector3::min(boundsMin, pos);
                boundsMax = Vector3::max(boundsMax, pos);
            }
        }

        PreparedGlbData::PrimitiveData pointCloudData(std::vector<PackedPointVertex>&& vertices,
            const Vector3& boundsMin, const Vector3& boundsMax, const int materialIndex)
        {
            PreparedGlbData::PrimitiveData pd;
            pd.pointCloud = true;
            pd.mode = TINYGLTF_MODE_POINTS;
            pd.materialIndex = materialIndex;
            pd.vertexCount = static_cast<int>(vertices.size());
            pd.drawCount = pd.vertexCount;
            pd.vertexBytes.resize(vertices.size() * sizeof(PackedPointVertex));
            std::memcpy(pd.vertexBytes.data(), vertices.data(), pd.vertexBytes.size());
            pd.boundsMin = boundsMin;
            pd.boundsMax = boundsMax;
            return pd;
        }
    }

    namespace
    {
        // The glTF triangle modes: a list, a strip and a fan.
        bool isTriangleMode(const int mode)
        {
            return mode == TINYGLTF_MODE_TRIANGLES || mode == TINYGLTF_MODE_TRIANGLE_STRIP ||
                mode == TINYGLTF_MODE_TRIANGLE_FAN;
        }

        // The triangles of a triangle-mode primitive as corner vertex indices, three per
        // triangle, in the order the glTF specification defines for each mode: a strip
        // swaps the last two corners of every odd triangle, so all of them face the way
        // the first one does, and a fan turns around its first vertex. `indices` empty
        // means the vertices in order. A triangle naming a vertex past the end is dropped.
        std::vector<uint32_t> triangleCorners(const int mode, const std::vector<uint32_t>& indices,
            const size_t vertexCount)
        {
            const size_t n = indices.empty() ? vertexCount : indices.size();
            const auto at = [&](const size_t i) {
                return indices.empty() ? static_cast<uint32_t>(i) : indices[i];
            };
            std::vector<uint32_t> corners;
            const auto add = [&](const uint32_t a, const uint32_t b, const uint32_t c) {
                if (a < vertexCount && b < vertexCount && c < vertexCount) {
                    corners.insert(corners.end(), {a, b, c});
                }
            };
            if (mode == TINYGLTF_MODE_TRIANGLES) {
                corners.reserve(n - n % 3);
                for (size_t i = 0; i + 2 < n; i += 3) {
                    add(at(i), at(i + 1), at(i + 2));
                }
            } else if (mode == TINYGLTF_MODE_TRIANGLE_STRIP) {
                for (size_t i = 0; i + 2 < n; ++i) {
                    const bool odd = (i & 1) != 0;
                    add(at(i), at(i + (odd ? 2 : 1)), at(i + (odd ? 1 : 2)));
                }
            } else if (mode == TINYGLTF_MODE_TRIANGLE_FAN) {
                for (size_t i = 1; i + 1 < n; ++i) {
                    add(at(i), at(i + 1), at(0));
                }
            }
            return corners;
        }

        // A triangle primitive without NORMAL is FLAT shaded, as the glTF specification
        // requires: every triangle gets three vertices of its own carrying its face
        // normal, cross(p1 - p0, p2 - p0) normalised (a zero-area triangle takes +Y).
        // On return the primitive is a non-indexed triangle list. Returns the source
        // vertex each resulting vertex was copied from, so the streams read beside the
        // vertices (skin influences, morph deltas, colours) can be re-indexed the same
        // way; empty when the vertices already were one triple per triangle and only
        // their normals changed.
        //
        // DEVIATION: upstream keeps smoothly averaged normals in the vertex buffer and
        // flat shades through a material flag (normals from screen-space derivatives),
        // which this engine's materials do not have. Unwelding gives the same facets
        // without a shader change, at the cost of three vertices per triangle, and the
        // flat look cannot be switched back to smooth normals afterwards.
        std::vector<uint32_t> applyFlatNormals(std::vector<PackedVertex>& vertices, std::vector<uint32_t>& indices,
            int& mode)
        {
            std::vector<uint32_t> corners = triangleCorners(mode, indices, vertices.size());
            const bool inPlace = mode == TINYGLTF_MODE_TRIANGLES && indices.empty() &&
                corners.size() == vertices.size();
            if (inPlace) {
                corners.clear();
            } else {
                std::vector<PackedVertex> unwelded;
                unwelded.reserve(corners.size());
                for (const uint32_t source : corners) {
                    unwelded.push_back(vertices[source]);
                }
                vertices = std::move(unwelded);
            }
            for (size_t t = 0; t + 2 < vertices.size(); t += 3) {
                const Vector3 p0 = Vector3::load(&vertices[t].px);
                const Vector3 faceNormal = (Vector3::load(&vertices[t + 1].px) - p0).cross(
                    Vector3::load(&vertices[t + 2].px) - p0);
                const float length = faceNormal.length();
                const Vector3 normal = length > 0.0f ? faceNormal * (1.0f / length) : Vector3(0.0f, 1.0f, 0.0f);
                for (size_t k = 0; k < 3; ++k) {
                    vertices[t + k].nx = normal.getX();
                    vertices[t + k].ny = normal.getY();
                    vertices[t + k].nz = normal.getZ();
                }
            }
            indices.clear();
            mode = TINYGLTF_MODE_TRIANGLES;
            return corners;
        }

        // Re-indexes a per-vertex stream of `width` values per vertex after
        // applyFlatNormals (`source` empty: nothing to do). A stream shorter than the
        // `sourceCount` vertices it should cover cannot be re-indexed and is cleared.
        void remapPerVertex(std::vector<float>& values, const std::vector<uint32_t>& source,
            const size_t sourceCount, const size_t width)
        {
            if (source.empty() || values.empty()) {
                return;
            }
            if (values.size() < sourceCount * width) {
                values.clear();
                return;
            }
            std::vector<float> remapped(source.size() * width);
            for (size_t i = 0; i < source.size(); ++i) {
                std::copy_n(values.data() + static_cast<size_t>(source[i]) * width, width, remapped.data() + i * width);
            }
            values = std::move(remapped);
        }

        // Interleaves the vertices with COLOR_0 (RGBA per vertex) into the 72-byte
        // vertex-coloured layout. glTF vertex colours are LINEAR, and so is what the
        // vertex stage passes on unless a material sets vertexColorGamma, which a
        // glTF material never does: the file's values are stored as they are. Alpha
        // is written as 1: the colour tints the diffuse only, and opacity is not taken
        // from it.
        std::vector<uint8_t> packColoredVertices(const std::vector<PackedVertex>& vertices,
            const std::vector<float>& colors)
        {
            std::vector<uint8_t> bytes(vertices.size() * COLORED_VERTEX_STRIDE);
            for (size_t i = 0; i < vertices.size(); ++i) {
                auto* dst = bytes.data() + i * COLORED_VERTEX_STRIDE;
                std::memcpy(dst, &vertices[i], sizeof(PackedVertex));
                float color[4];
                for (size_t c = 0; c < 3; ++c) {
                    color[c] = std::max(colors[i * 4 + c], 0.0f);
                }
                color[3] = 1.0f;
                std::memcpy(dst + sizeof(PackedVertex), color, sizeof(color));
            }
            return bytes;
        }
    }

    namespace gltf
    {
        // Vertices, indices, skin attributes and morph targets of one non-POINTS
        // primitive. False when the primitive has nothing drawable.
        //
        // Every attribute is read through AccessorReader (sparse and quantised data
        // included). A triangle primitive without NORMAL is flat shaded
        // (applyFlatNormals), and the tangents a primitive without TANGENT gets are
        // derived afterwards, from the final normals. COLOR_0 takes the 72-byte
        // vertex-coloured layout on a static mesh; on a skinned or morphed one it is
        // dropped with a warning, and a colour that is white everywhere is dropped
        // silently, since it multiplies by one and would only cost the mesh its
        // batchable layout.
        bool extractTrianglePrimitive(const tinygltf::Model& model, const tinygltf::Mesh& mesh,
            const tinygltf::Primitive& primitive, const size_t meshIndex, PreparedGlbData& counters,
            PreparedGlbData::PrimitiveData& pd)
        {
            // An absent mode is TRIANGLES (the specification's default).
            int mode = primitive.mode < 0 ? TINYGLTF_MODE_TRIANGLES : primitive.mode;
            pd.materialIndex = primitive.material;
            pd.variantMaterials = primitiveVariantMaterials(primitive);

            std::vector<PackedVertex> vertices;
            std::vector<uint32_t> parsedIndices;
            std::vector<float> colors;   // RGBA per vertex, empty without COLOR_0
            bool hasNormals = false;
            bool hasTangents = false;
            Vector3 minPos(std::numeric_limits<float>::max());
            Vector3 maxPos(std::numeric_limits<float>::lowest());

            bool decodedDraco = false;
            if (primitiveUsesDraco(primitive)) {
                counters.dracoPrimitiveCount++;
                decodedDraco = decodeDracoPrimitive(model, primitive, vertices, parsedIndices, minPos, maxPos,
                    hasNormals, hasTangents, colors);
                if (!decodedDraco) {
                    counters.dracoDecodeFailureCount++;
                    spdlog::warn("Skipping glTF primitive due to Draco decode failure (mesh={})", meshIndex);
                    return false;
                }
                counters.dracoDecodeSuccessCount++;
                // A Draco payload is always a triangle list.
                mode = TINYGLTF_MODE_TRIANGLES;
            }

            if (!decodedDraco) {
                const auto* positionAccessor = readablePositions(model, primitive);
                if (!positionAccessor) {
                    return false;
                }
                const AccessorReader positions(model, *positionAccessor, TINYGLTF_TYPE_VEC3);
                if (!positions.valid()) {
                    spdlog::warn("Skipping glTF primitive with unreadable POSITION data (mesh={})", meshIndex);
                    return false;
                }
                const auto vertexCount = positions.count();
                // An attribute that cannot be read, or covers fewer vertices than
                // POSITION, counts as absent.
                const auto attribute = [&](const char* name, const int type) {
                    const auto* accessor = primitiveAttribute(model, primitive, name);
                    if (!accessor) {
                        return AccessorReader();
                    }
                    AccessorReader reader(model, *accessor, type);
                    if (!reader.valid() || reader.count() < vertexCount) {
                        return AccessorReader();
                    }
                    return reader;
                };
                const AccessorReader normals = attribute("NORMAL", TINYGLTF_TYPE_VEC3);
                const AccessorReader uvs = attribute("TEXCOORD_0", TINYGLTF_TYPE_VEC2);
                const AccessorReader uvs1 = attribute("TEXCOORD_1", TINYGLTF_TYPE_VEC2);
                const AccessorReader tangents = attribute("TANGENT", TINYGLTF_TYPE_VEC4);
                hasNormals = normals.valid();
                hasTangents = tangents.valid();

                // COLOR_0 is VEC3 or VEC4; a VEC3 colour reads with alpha 1.
                AccessorReader colorReader = attribute("COLOR_0", TINYGLTF_TYPE_VEC4);
                if (!colorReader.valid()) {
                    colorReader = attribute("COLOR_0", TINYGLTF_TYPE_VEC3);
                }
                if (colorReader.valid()) {
                    colors.assign(vertexCount * 4, 1.0f);
                }

                vertices.resize(vertexCount);
                for (size_t i = 0; i < vertexCount; ++i) {
                    float pos[3] = {0.0f, 0.0f, 0.0f};
                    positions.read(i, pos, 3);
                    // Overwritten by generated face normals when the file has none.
                    float normal[3] = {0.0f, 1.0f, 0.0f};
                    normals.read(i, normal, 3);
                    // glTF's UV origin is the image's top-left corner, as every texture's
                    // here is, so UVs and images are both taken as the file has them.
                    float uv[2] = {0.0f, 0.0f};
                    uvs.read(i, uv, 2);
                    float uv1[2] = {uv[0], uv[1]};
                    uvs1.read(i, uv1, 2);
                    // Leave the tangent zero when the file carries none; triangle
                    // primitives get one generated below (generateTangents), and the
                    // shaders skip normal mapping on a degenerate tangent rather than
                    // building a bogus fixed basis. There is no derivative-based TBN
                    // fallback in this port.
                    float tangent[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                    // Taken as the file has it: glTF's bitangent, cross(N, T) * w, points
                    // toward the image's top row, as the engine's does.
                    tangents.read(i, tangent, 4);
                    if (!colors.empty()) {
                        colorReader.read(i, colors.data() + i * 4, colorReader.components());
                    }
                    vertices[i] = PackedVertex{
                        pos[0], pos[1], pos[2],
                        normal[0], normal[1], normal[2],
                        uv[0], uv[1],
                        tangent[0], tangent[1], tangent[2], tangent[3],
                        uv1[0], uv1[1]
                    };
                    const Vector3 position = Vector3::load(pos);
                    minPos = Vector3::min(minPos, position);
                    maxPos = Vector3::max(maxPos, position);
                }

                if (primitive.indices >= 0) {
                    if (const auto* indexAccessor = getAccessor(model, primitive.indices)) {
                        readIndices(model, *indexAccessor, parsedIndices);
                    }
                }
            }

            if (vertices.empty()) {
                return false;
            }
            const size_t sourceVertexCount = vertices.size();

            // Flat shading for a triangle primitive without normals, then tangents for
            // one without tangents, derived from the normals the vertices now carry.
            std::vector<uint32_t> unweldSource;
            if (!hasNormals && isTriangleMode(mode)) {
                unweldSource = applyFlatNormals(vertices, parsedIndices, mode);
                if (vertices.empty()) {
                    return false;
                }
                remapPerVertex(colors, unweldSource, sourceVertexCount, 4);
            }
            pd.authoredTangents = hasTangents;
            if (!hasTangents && mode == TINYGLTF_MODE_TRIANGLES) {
                generateTangents(vertices, parsedIndices);
                // The generator points the bitangent toward increasing v; the engine's
                // (and glTF's) points toward decreasing v, the image's top row.
                for (auto& vertex : vertices) {
                    vertex.tw = -vertex.tw;
                }
            }
            pd.mode = mode;

            // GPU skinning: JOINTS_0/WEIGHTS_0 present → the 88-byte skinned layout
            // (the Draco path never carries skin attributes here).
            pd.vertexCount = static_cast<int>(vertices.size());
            auto skinAttributes = decodedDraco
                ? SkinAttributes{} : readSkinAttributes(model, primitive, sourceVertexCount);
            if (skinAttributes.valid && !unweldSource.empty()) {
                remapPerVertex(skinAttributes.weights, unweldSource, sourceVertexCount, 4);
                remapPerVertex(skinAttributes.joints, unweldSource, sourceVertexCount, 4);
            }

            // Morph targets (skipped for Draco primitives — vertex order differs).
            if (!decodedDraco && !primitive.targets.empty()) {
                pd.morphTargets = readMorphTargets(model, primitive, sourceVertexCount);
                pd.morphInitialWeights.assign(mesh.weights.begin(), mesh.weights.end());
                for (auto& target : pd.morphTargets) {
                    remapPerVertex(target.deltaPositions, unweldSource, sourceVertexCount, 3);
                    remapPerVertex(target.deltaNormals, unweldSource, sourceVertexCount, 3);
                }
            }

            bool coloursMatter = false;
            for (size_t i = 0; i + 3 < colors.size() && !coloursMatter; i += 4) {
                coloursMatter = colors[i] != 1.0f || colors[i + 1] != 1.0f || colors[i + 2] != 1.0f;
            }
            if (coloursMatter && (skinAttributes.valid || !pd.morphTargets.empty())) {
                static std::atomic<bool> warned{false};
                if (!warned.exchange(true)) {
                    spdlog::warn("GLB: COLOR_0 on a skinned or morphed primitive is not supported and is "
                        "ignored (mesh={}); further cases are not reported", meshIndex);
                }
            }

            if (skinAttributes.valid) {
                pd.skinned = true;
                pd.vertexBytes = packSkinnedVertices(vertices, skinAttributes);
            } else if (coloursMatter && pd.morphTargets.empty()) {
                pd.vertexColors = true;
                pd.vertexBytes = packColoredVertices(vertices, colors);
            } else {
                pd.vertexBytes.resize(vertices.size() * sizeof(PackedVertex));
                std::memcpy(pd.vertexBytes.data(), vertices.data(), pd.vertexBytes.size());
            }

            pd.drawCount = static_cast<int>(vertices.size());
            if (!parsedIndices.empty()) {
                pd.indexBytes.resize(parsedIndices.size() * sizeof(uint32_t));
                std::memcpy(pd.indexBytes.data(), parsedIndices.data(), pd.indexBytes.size());
                pd.drawCount = static_cast<int>(parsedIndices.size());
                pd.indexed = true;
            }
            pd.boundsMin = minPos;
            pd.boundsMax = maxPos;
            return true;
        }

        std::shared_ptr<Mesh> createPreparedMesh(PreparedGlbData::PrimitiveData& pd,
            const std::shared_ptr<GraphicsDevice>& device, const std::shared_ptr<VertexFormat>& format)
        {
            VertexBufferOptions vbOptions;
            vbOptions.data = std::move(pd.vertexBytes);
            auto vertexBuffer = device->createVertexBuffer(format, pd.vertexCount, vbOptions);
            if (!vertexBuffer) {
                return nullptr;
            }

            std::shared_ptr<IndexBuffer> indexBuffer;
            if (pd.indexed && !pd.indexBytes.empty()) {
                const int indexCount = static_cast<int>(pd.indexBytes.size() / sizeof(uint32_t));
                indexBuffer = device->createIndexBuffer(INDEXFORMAT_UINT32, indexCount, pd.indexBytes);
            }

            auto mesh = std::make_shared<Mesh>();
            mesh->setVertexBuffer(vertexBuffer);
            mesh->setIndexBuffer(indexBuffer, 0);

            Primitive drawPrimitive;
            drawPrimitive.type = pd.pointCloud ? PRIMITIVE_POINTS : mapPrimitiveType(pd.mode);
            drawPrimitive.base = 0;
            drawPrimitive.baseVertex = 0;
            drawPrimitive.count = pd.drawCount;
            drawPrimitive.indexed = pd.indexed && indexBuffer;
            mesh->setPrimitive(drawPrimitive, 0);

            BoundingBox bounds;
            bounds.setCenter((pd.boundsMin + pd.boundsMax) * 0.5f);
            bounds.setHalfExtents((pd.boundsMax - pd.boundsMin) * 0.5f);
            mesh->setAabb(bounds);
            return mesh;
        }
    }
}
