// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// glTF parser: EXT_ / KHR_meshopt_compression (fallback buffers and decoding).
// The two extensions share their JSON schema; KHR adds the v1 vertex bitstream and the
// COLOR filter, both of which meshoptimizer's decoder reads, so they are handled alike.
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

#include <meshoptimizer.h>
#include <nlohmann/json.hpp>

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
        const nlohmann::json* meshoptCompression(const nlohmann::json& object)
        {
            const auto extensions = object.find("extensions");
            if (extensions == object.end() || !extensions->is_object()) {
                return nullptr;
            }
            for (const char* name : {"KHR_meshopt_compression", "EXT_meshopt_compression"}) {
                if (const auto it = extensions->find(name); it != extensions->end() && it->is_object()) {
                    return &*it;
                }
            }
            return nullptr;
        }
    }

    namespace gltf
    {
        // The glTF JSON with every meshopt FALLBACK buffer replaced by an embedded one-byte
        // buffer, or nothing when no buffer needs it. A fallback buffer only stands in for
        // the decoded views: one without a uri has no data (unless it is a GLB's binary
        // chunk, buffer 0), and one with a uri names a file that need not exist. tinygltf
        // would read the first from the binary chunk, refusing the file when the
        // uncompressed size exceeds the chunk, and fail on the second.
        std::optional<std::string> withoutMeshoptFallbackBuffers(const std::string_view json, const bool binary)
        {
            if (json.find("_meshopt_compression") == std::string_view::npos) {
                return std::nullopt;
            }
            auto document = nlohmann::json::parse(json, nullptr, false);
            if (document.is_discarded() || !document.is_object()) {
                return std::nullopt;
            }
            const auto buffers = document.find("buffers");
            if (buffers == document.end() || !buffers->is_array()) {
                return std::nullopt;
            }
            bool changed = false;
            for (size_t i = 0; i < buffers->size(); ++i) {
                auto& buffer = (*buffers)[i];
                const auto* compression = buffer.is_object() ? meshoptCompression(buffer) : nullptr;
                if (!compression || compression->value("fallback", false) != true) {
                    continue;
                }
                if (binary && i == 0 && !buffer.contains("uri")) {
                    continue;   // the GLB binary chunk: real data
                }
                buffer["uri"] = "data:application/octet-stream;base64,AA==";
                buffer["byteLength"] = 1;
                changed = true;
            }
            if (!changed) {
                return std::nullopt;
            }
            return document.dump();
        }

        // A GLB container with its JSON chunk replaced and every later chunk kept.
        std::optional<std::vector<std::uint8_t>> withGlbJson(const std::uint8_t* data, const size_t length,
            const std::string& json)
        {
            const auto readU32 = [&](const size_t at) {
                std::uint32_t value;
                std::memcpy(&value, data + at, sizeof(value));
                return value;
            };
            const auto writeU32 = [](std::vector<std::uint8_t>& out, const std::uint32_t value) {
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
                out.insert(out.end(), bytes, bytes + sizeof(value));
            };
            constexpr std::uint32_t kJsonChunk = 0x4E4F534Au;
            if (length < 20 || readU32(12 + 4) != kJsonChunk) {
                return std::nullopt;
            }
            const size_t restAt = 20 + static_cast<size_t>(readU32(12));
            if (restAt > length) {
                return std::nullopt;
            }
            const size_t paddedJson = (json.size() + 3) & ~size_t{3};
            std::vector<std::uint8_t> out;
            out.reserve(20 + paddedJson + (length - restAt));
            out.insert(out.end(), data, data + 8);   // magic, version
            writeU32(out, static_cast<std::uint32_t>(20 + paddedJson + (length - restAt)));
            writeU32(out, static_cast<std::uint32_t>(paddedJson));
            writeU32(out, kJsonChunk);
            out.insert(out.end(), json.begin(), json.end());
            out.resize(20 + paddedJson, static_cast<std::uint8_t>(' '));
            out.insert(out.end(), data + restAt, data + length);
            return out;
        }
    }

    namespace
    {
        double meshoptNumber(const tinygltf::Value& extension, const char* name, const double fallback)
        {
            if (!extension.Has(name)) {
                return fallback;
            }
            const auto& value = extension.Get(name);
            return value.IsNumber() ? value.GetNumberAsDouble() : fallback;
        }

        std::string meshoptString(const tinygltf::Value& extension, const char* name, const char* fallback)
        {
            if (!extension.Has(name) || !extension.Get(name).IsString()) {
                return fallback;
            }
            return extension.Get(name).Get<std::string>();
        }

        // Decodes one compressed view; empty on failure, with `why` saying what failed.
        std::vector<unsigned char> decodeMeshoptView(const tinygltf::Model& model,
            const tinygltf::Value& compression, std::string& why)
        {
            const auto bufferIndex = static_cast<long long>(meshoptNumber(compression, "buffer", -1.0));
            const auto byteOffset = static_cast<size_t>(meshoptNumber(compression, "byteOffset", 0.0));
            const auto byteLength = static_cast<size_t>(meshoptNumber(compression, "byteLength", 0.0));
            const auto stride = static_cast<size_t>(meshoptNumber(compression, "byteStride", 0.0));
            const auto count = static_cast<size_t>(meshoptNumber(compression, "count", 0.0));
            const std::string mode = meshoptString(compression, "mode", "");
            const std::string filter = meshoptString(compression, "filter", "NONE");

            if (bufferIndex < 0 || static_cast<size_t>(bufferIndex) >= model.buffers.size()) {
                why = "names no buffer";
                return {};
            }
            const auto& source = model.buffers[static_cast<size_t>(bufferIndex)].data;
            if (byteOffset > source.size() || byteLength > source.size() - byteOffset) {
                why = "reaches past its buffer";
                return {};
            }
            // The decoders assert on these; a malformed file must not reach them.
            const bool indices = mode == "TRIANGLES" || mode == "INDICES";
            if (indices ? (stride != 2 && stride != 4) : (stride == 0 || stride > 256 || stride % 4 != 0)) {
                why = "has an invalid byteStride";
                return {};
            }
            if (mode == "TRIANGLES" && count % 3 != 0) {
                why = "has a TRIANGLES count that is not a multiple of 3";
                return {};
            }

            std::vector<unsigned char> decoded(count * stride);
            const unsigned char* bytes = source.data() + byteOffset;
            int result = -1;
            if (mode == "ATTRIBUTES") {
                result = meshopt_decodeVertexBuffer(decoded.data(), count, stride, bytes, byteLength);
            } else if (mode == "TRIANGLES") {
                result = meshopt_decodeIndexBuffer(decoded.data(), count, stride, bytes, byteLength);
            } else if (mode == "INDICES") {
                result = meshopt_decodeIndexSequence(decoded.data(), count, stride, bytes, byteLength);
            } else {
                why = "has unknown mode '" + mode + "'";
                return {};
            }
            if (result != 0) {
                why = "did not decode (mode " + mode + ")";
                return {};
            }

            if (filter == "NONE") {
                return decoded;
            }
            if (mode != "ATTRIBUTES") {
                why = "filters a " + mode + " view";
                return {};
            }
            if (filter == "OCTAHEDRAL" && (stride == 4 || stride == 8)) {
                meshopt_decodeFilterOct(decoded.data(), count, stride);
            } else if (filter == "QUATERNION" && stride == 8) {
                meshopt_decodeFilterQuat(decoded.data(), count, stride);
            } else if (filter == "EXPONENTIAL") {
                meshopt_decodeFilterExp(decoded.data(), count, stride);
            } else if (filter == "COLOR" && (stride == 4 || stride == 8)) {
                meshopt_decodeFilterColor(decoded.data(), count, stride);
            } else {
                why = "has filter " + filter + " at byteStride " + std::to_string(stride);
                return {};
            }
            return decoded;
        }
    }

    namespace gltf
    {
        // Decodes every meshopt compressed buffer view into a buffer of its own and points
        // the view at it, so accessors, Draco and animation read it as an ordinary view.
        void decodeMeshoptBufferViews(tinygltf::Model& model, const std::string& debugName)
        {
            for (size_t i = 0; i < model.bufferViews.size(); ++i) {
                auto& view = model.bufferViews[i];
                auto extension = view.extensions.find("KHR_meshopt_compression");
                if (extension == view.extensions.end()) {
                    extension = view.extensions.find("EXT_meshopt_compression");
                }
                if (extension == view.extensions.end()) {
                    continue;
                }
                std::string why;
                auto decoded = decodeMeshoptView(model, extension->second, why);
                view.extensions.erase(extension);
                if (decoded.empty() && !why.empty()) {
                    spdlog::error("glTF [{}]: meshopt compressed buffer view {} {}", debugName, i, why);
                    continue;
                }
                tinygltf::Buffer buffer;
                buffer.data = std::move(decoded);
                view.buffer = static_cast<int>(model.buffers.size());
                view.byteOffset = 0;
                view.byteLength = buffer.data.size();
                model.buffers.push_back(std::move(buffer));
            }
        }
    }
}
