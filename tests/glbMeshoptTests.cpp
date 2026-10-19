// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.10.2026
//
// EXT_meshopt_compression and KHR_meshopt_compression: a file whose buffer views are
// meshopt compressed must load to the same geometry as the same file stored plain.
//
// One 32 x 32 grid is written twice, as a GLB with plain buffer views and as a GLB whose
// views are compressed into the binary chunk behind a FALLBACK buffer that has no data
// (what gltfpack writes), and once more as glTF JSON whose fallback buffer names a file
// that does not exist. Every mode is used (ATTRIBUTES, TRIANGLES, INDICES) and every
// filter (OCTAHEDRAL normals, EXPONENTIAL uvs, COLOR colours, QUATERNION in a view
// nothing references), split between the EXT and KHR names. The fallback buffer is
// larger than the binary chunk, which tinygltf refuses when it reads the buffer itself.
//
// Lossless streams (positions, both index modes) must come back byte for byte; filtered
// ones are compared with the source values at the filter's precision.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <meshoptimizer.h>

#include "framework/parsers/glbContainerResource.h"
#include "framework/parsers/packedVertex.h"
#include "support/check.h"
#include "support/gltfModel.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr int kGrid = 32;
    constexpr int kVertices = kGrid * kGrid;
    constexpr int kQuaternions = 64;

    struct Source
    {
        std::vector<float> positions;    // xyz
        std::vector<float> normals;      // xyz0
        std::vector<float> uvs;          // uv
        std::vector<float> colors;       // rgba in [0, 1], multiples of 1/255
        std::vector<uint16_t> indices;
        std::vector<float> quaternions;  // xyzw, unit
    };

    Source makeSource()
    {
        Source s;
        for (int y = 0; y < kGrid; ++y) {
            for (int x = 0; x < kGrid; ++x) {
                const float fx = static_cast<float>(x) / (kGrid - 1);
                const float fy = static_cast<float>(y) / (kGrid - 1);
                const float h = 0.3f * std::sin(fx * 6.0f) * std::cos(fy * 5.0f);
                s.positions.insert(s.positions.end(), {fx * 4.0f - 2.0f, h, fy * 4.0f - 2.0f});
                const float dx = -0.3f * 6.0f * std::cos(fx * 6.0f) * std::cos(fy * 5.0f) / 4.0f;
                const float dz = 0.3f * 5.0f * std::sin(fx * 6.0f) * std::sin(fy * 5.0f) / 4.0f;
                const float length = std::sqrt(dx * dx + 1.0f + dz * dz);
                s.normals.insert(s.normals.end(), {dx / length, 1.0f / length, dz / length, 0.0f});
                s.uvs.insert(s.uvs.end(), {fx * 3.0f, fy * 2.0f + 0.125f});
                s.colors.insert(s.colors.end(), {static_cast<float>(x * 8) / 255.0f,
                    static_cast<float>(y * 8) / 255.0f, 200.0f / 255.0f, 1.0f});
            }
        }
        for (int y = 0; y + 1 < kGrid; ++y) {
            for (int x = 0; x + 1 < kGrid; ++x) {
                const auto i = static_cast<uint16_t>(y * kGrid + x);
                s.indices.insert(s.indices.end(), {i, static_cast<uint16_t>(i + kGrid), static_cast<uint16_t>(i + 1),
                    static_cast<uint16_t>(i + 1), static_cast<uint16_t>(i + kGrid),
                    static_cast<uint16_t>(i + kGrid + 1)});
            }
        }
        for (int i = 0; i < kQuaternions; ++i) {
            const float angle = 0.1f * static_cast<float>(i);
            const float ax = 0.6f, ay = 0.0f, az = 0.8f;
            s.quaternions.insert(s.quaternions.end(), {ax * std::sin(angle / 2), ay * std::sin(angle / 2),
                az * std::sin(angle / 2), std::cos(angle / 2)});
        }
        return s;
    }

    // One buffer view's bytes as the plain file stores them, and how to compress them.
    struct Stream
    {
        std::vector<uint8_t> plain;      // what a plain view holds (the filters' output format)
        std::vector<uint8_t> encoded;    // the input of the vertex / index codec
        size_t stride = 0;
        size_t count = 0;
        const char* mode = "ATTRIBUTES";
        const char* filter = "NONE";
        const char* extension = "KHR_meshopt_compression";
        size_t target = 0;               // 34962 vertices, 34963 indices, 0 none
    };

    std::vector<uint8_t> bytesOf(const void* data, const size_t size)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        return {p, p + size};
    }

    std::vector<Stream> makeStreams(const Source& s)
    {
        std::vector<Stream> streams(7);

        auto& positions = streams[0];
        positions.plain = bytesOf(s.positions.data(), s.positions.size() * sizeof(float));
        positions.encoded = positions.plain;
        positions.stride = 12;
        positions.count = kVertices;
        positions.extension = "EXT_meshopt_compression";
        positions.target = 34962;

        // The filtered streams: the plain file holds what the decoder's filter outputs,
        // quantised the same way, so both files describe one mesh.
        auto& normals = streams[1];
        normals.encoded.resize(kVertices * 4);
        meshopt_encodeFilterOct(normals.encoded.data(), kVertices, 4, 8, s.normals.data());
        normals.plain = normals.encoded;
        meshopt_decodeFilterOct(normals.plain.data(), kVertices, 4);
        normals.stride = 4;
        normals.count = kVertices;
        normals.filter = "OCTAHEDRAL";
        normals.target = 34962;

        auto& uvs = streams[2];
        uvs.encoded.resize(kVertices * 8);
        meshopt_encodeFilterExp(uvs.encoded.data(), kVertices, 8, 24, s.uvs.data(), meshopt_EncodeExpSeparate);
        uvs.plain = uvs.encoded;
        meshopt_decodeFilterExp(uvs.plain.data(), kVertices, 8);
        uvs.stride = 8;
        uvs.count = kVertices;
        uvs.filter = "EXPONENTIAL";
        uvs.extension = "EXT_meshopt_compression";
        uvs.target = 34962;

        auto& colors = streams[3];
        colors.encoded.resize(kVertices * 4);
        meshopt_encodeFilterColor(colors.encoded.data(), kVertices, 4, 8, s.colors.data());
        colors.plain = colors.encoded;
        meshopt_decodeFilterColor(colors.plain.data(), kVertices, 4);
        colors.stride = 4;
        colors.count = kVertices;
        colors.filter = "COLOR";
        colors.target = 34962;

        auto& triangles = streams[4];
        triangles.plain = bytesOf(s.indices.data(), s.indices.size() * sizeof(uint16_t));
        triangles.encoded = triangles.plain;
        triangles.stride = 2;
        triangles.count = s.indices.size();
        triangles.mode = "TRIANGLES";
        triangles.extension = "EXT_meshopt_compression";
        triangles.target = 34963;

        auto& sequence = streams[5];
        sequence = triangles;
        sequence.mode = "INDICES";
        sequence.extension = "KHR_meshopt_compression";

        auto& quaternions = streams[6];
        quaternions.encoded.resize(kQuaternions * 8);
        meshopt_encodeFilterQuat(quaternions.encoded.data(), kQuaternions, 8, 12, s.quaternions.data());
        quaternions.plain = quaternions.encoded;
        meshopt_decodeFilterQuat(quaternions.plain.data(), kQuaternions, 8);
        quaternions.stride = 8;
        quaternions.count = kQuaternions;
        quaternions.filter = "QUATERNION";
        return streams;
    }

    std::vector<uint8_t> compress(const Stream& stream)
    {
        std::vector<uint8_t> out;
        if (std::string(stream.mode) == "ATTRIBUTES") {
            out.resize(meshopt_encodeVertexBufferBound(stream.count, stream.stride));
            out.resize(meshopt_encodeVertexBuffer(out.data(), out.size(), stream.encoded.data(), stream.count,
                stream.stride));
            return out;
        }
        const auto* indices = reinterpret_cast<const uint16_t*>(stream.encoded.data());
        const std::vector<unsigned> wide(indices, indices + stream.count);
        out.resize(meshopt_encodeIndexBufferBound(stream.count, kVertices));
        const bool triangles = std::string(stream.mode) == "TRIANGLES";
        out.resize(triangles ? meshopt_encodeIndexBuffer(out.data(), out.size(), wide.data(), wide.size())
                             : meshopt_encodeIndexSequence(out.data(), out.size(), wide.data(), wide.size()));
        return out;
    }

    void append(std::vector<uint8_t>& to, const std::vector<uint8_t>& bytes)
    {
        to.insert(to.end(), bytes.begin(), bytes.end());
        to.resize((to.size() + 3) & ~size_t{3});
    }

    std::string base64(const std::vector<uint8_t>& bytes)
    {
        static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (size_t i = 0; i < bytes.size(); i += 3) {
            const uint32_t n = (uint32_t{bytes[i]} << 16) | (i + 1 < bytes.size() ? uint32_t{bytes[i + 1]} << 8 : 0u) |
                (i + 2 < bytes.size() ? uint32_t{bytes[i + 2]} : 0u);
            out += table[(n >> 18) & 63];
            out += table[(n >> 12) & 63];
            out += i + 1 < bytes.size() ? table[(n >> 6) & 63] : '=';
            out += i + 2 < bytes.size() ? table[n & 63] : '=';
        }
        return out;
    }

    enum class Kind
    {
        Plain,
        CompressedGlb,
        CompressedGltf,
    };

    // The glTF JSON and its binary chunk (empty for glTF JSON, which embeds its data).
    std::pair<std::string, std::vector<uint8_t>> build(const std::vector<Stream>& streams, const Kind kind)
    {
        std::vector<uint8_t> plainBytes;
        std::vector<size_t> plainOffsets;
        for (const auto& stream : streams) {
            plainOffsets.push_back(plainBytes.size());
            append(plainBytes, stream.plain);
        }
        std::vector<uint8_t> packed;
        std::vector<std::pair<size_t, size_t>> packedRanges;
        for (const auto& stream : streams) {
            const auto compressed = compress(stream);
            packedRanges.emplace_back(packed.size(), compressed.size());
            append(packed, compressed);
        }

        std::string views;
        for (size_t i = 0; i < streams.size(); ++i) {
            const auto& stream = streams[i];
            std::string view = "{\"buffer\":" + std::string(kind == Kind::Plain ? "0" : "1") +
                ",\"byteOffset\":" + std::to_string(plainOffsets[i]) +
                ",\"byteLength\":" + std::to_string(stream.plain.size());
            if (stream.target == 34962) {
                view += ",\"byteStride\":" + std::to_string(stream.stride);
            }
            if (stream.target != 0) {
                view += ",\"target\":" + std::to_string(stream.target);
            }
            if (kind != Kind::Plain) {
                view += ",\"extensions\":{\"" + std::string(stream.extension) + "\":{\"buffer\":0,\"byteOffset\":" +
                    std::to_string(packedRanges[i].first) + ",\"byteLength\":" +
                    std::to_string(packedRanges[i].second) + ",\"byteStride\":" + std::to_string(stream.stride) +
                    ",\"count\":" + std::to_string(stream.count) + ",\"mode\":\"" + stream.mode + "\"" +
                    (std::string(stream.filter) != "NONE" ? ",\"filter\":\"" + std::string(stream.filter) + "\"" : "") +
                    "}}";
            }
            view += "}";
            views += (i ? "," : "") + view;
        }

        std::string buffers;
        std::vector<uint8_t> binary;
        if (kind == Kind::Plain) {
            buffers = "{\"byteLength\":" + std::to_string(plainBytes.size()) + "}";
            binary = plainBytes;
        } else {
            const std::string fallback = "\"byteLength\":" + std::to_string(plainBytes.size()) +
                ",\"extensions\":{\"KHR_meshopt_compression\":{\"fallback\":true}}";
            if (kind == Kind::CompressedGlb) {
                buffers = "{\"byteLength\":" + std::to_string(packed.size()) + "},{" + fallback + "}";
                binary = packed;
            } else {
                buffers = "{\"byteLength\":" + std::to_string(packed.size()) +
                    ",\"uri\":\"data:application/octet-stream;base64," + base64(packed) + "\"}," +
                    "{\"uri\":\"visutwin-meshopt-missing.fallback.bin\"," + fallback + "}";
            }
        }

        const std::string extensions = kind == Kind::Plain
            ? "\"extensionsUsed\":[\"KHR_mesh_quantization\"],\"extensionsRequired\":[\"KHR_mesh_quantization\"],"
            : "\"extensionsUsed\":[\"KHR_mesh_quantization\",\"EXT_meshopt_compression\",\"KHR_meshopt_compression\"],"
              "\"extensionsRequired\":[\"KHR_mesh_quantization\",\"EXT_meshopt_compression\",\"KHR_meshopt_compression\"],";
        const std::string attributes = "{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2,\"COLOR_0\":3}";
        const std::string json = "{\"asset\":{\"version\":\"2.0\"}," + extensions +
            "\"buffers\":[" + buffers + "],\"bufferViews\":[" + views + "],"
            "\"accessors\":["
            "{\"bufferView\":0,\"componentType\":5126,\"count\":" + std::to_string(kVertices) +
            ",\"type\":\"VEC3\",\"min\":[-2,-0.3,-2],\"max\":[2,0.3,2]},"
            "{\"bufferView\":1,\"componentType\":5120,\"normalized\":true,\"count\":" + std::to_string(kVertices) +
            ",\"type\":\"VEC3\"},"
            "{\"bufferView\":2,\"componentType\":5126,\"count\":" + std::to_string(kVertices) + ",\"type\":\"VEC2\"},"
            "{\"bufferView\":3,\"componentType\":5121,\"normalized\":true,\"count\":" + std::to_string(kVertices) +
            ",\"type\":\"VEC4\"},"
            "{\"bufferView\":4,\"componentType\":5123,\"count\":" + std::to_string(streams[4].count) +
            ",\"type\":\"SCALAR\"},"
            "{\"bufferView\":5,\"componentType\":5123,\"count\":" + std::to_string(streams[5].count) +
            ",\"type\":\"SCALAR\"}],"
            "\"meshes\":[{\"primitives\":["
            "{\"attributes\":" + attributes + ",\"indices\":4},"
            "{\"attributes\":" + attributes + ",\"indices\":5}]}],"
            "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
        return {json, binary};
    }

    std::vector<uint8_t> glb(std::string json, const std::vector<uint8_t>& binary)
    {
        json.resize((json.size() + 3) & ~size_t{3}, ' ');
        std::vector<uint8_t> out;
        const auto u32 = [&](const uint32_t v) { out.insert(out.end(), reinterpret_cast<const uint8_t*>(&v),
            reinterpret_cast<const uint8_t*>(&v) + 4); };
        u32(0x46546C67u);
        u32(2);
        u32(static_cast<uint32_t>(12 + 8 + json.size() + 8 + binary.size()));
        u32(static_cast<uint32_t>(json.size()));
        u32(0x4E4F534Au);
        out.insert(out.end(), json.begin(), json.end());
        u32(static_cast<uint32_t>(binary.size()));
        u32(0x004E4942u);
        out.insert(out.end(), binary.begin(), binary.end());
        return out;
    }

    bool load(const Kind kind, const std::vector<Stream>& streams, tinygltf::Model& model)
    {
        const auto [json, binary] = build(streams, kind);
        std::string err, warn;
        if (kind == Kind::CompressedGltf) {
            return GlbParser::loadModel(model, reinterpret_cast<const uint8_t*>(json.data()), json.size(), false,
                std::string(), err, warn) || (std::cout << "  tinygltf: " << err << "\n", false);
        }
        const auto bytes = glb(json, binary);
        return GlbParser::loadModel(model, bytes.data(), bytes.size(), true, std::string(), err, warn) ||
            (std::cout << "  tinygltf: " << err << "\n", false);
    }

    const PackedVertex& vertexAt(const PreparedGlbData::PrimitiveData& primitive, const int i)
    {
        return *reinterpret_cast<const PackedVertex*>(primitive.vertexBytes.data() + static_cast<size_t>(i) * 72);
    }

    const float* colorAt(const PreparedGlbData::PrimitiveData& primitive, const int i)
    {
        return reinterpret_cast<const float*>(primitive.vertexBytes.data() + static_cast<size_t>(i) * 72 + 56);
    }

    void compare(const char* name, const Kind kind, const Source& source, const std::vector<Stream>& streams,
        const PreparedGlbData& plain)
    {
        std::cout << name << "\n";
        tinygltf::Model model;
        if (!check(load(kind, streams, model), "  the file loads")) {
            return;
        }
        const auto prepared = GlbParser::prepareFromModel(model, PixelFormat::PIXELFORMAT_RGBA8, name);

        // The views are decoded in the model itself, so the unreferenced quaternion view
        // can be read there.
        const auto& view = model.bufferViews[6];
        const auto& bytes = model.buffers[static_cast<size_t>(view.buffer)].data;
        bool quaternions = view.extensions.empty() && view.byteLength == kQuaternions * 8u &&
            bytes.size() >= view.byteOffset + view.byteLength;
        for (int i = 0; quaternions && i < kQuaternions; ++i) {
            int16_t q[4];
            std::memcpy(q, bytes.data() + view.byteOffset + static_cast<size_t>(i) * 8, sizeof(q));
            float dot = 0.0f;
            for (int c = 0; c < 4; ++c) {
                dot += static_cast<float>(q[c]) / 32767.0f * source.quaternions[static_cast<size_t>(i * 4 + c)];
            }
            quaternions = std::abs(dot) > 0.9995f;
        }
        check(quaternions, "  QUATERNION: the unreferenced view decodes to its unit quaternions");

        if (!check(prepared.meshPrimitives.size() == 1 && prepared.meshPrimitives[0].size() == 2,
                "  one mesh of two primitives")) {
            return;
        }
        for (int p = 0; p < 2; ++p) {
            const auto& got = prepared.meshPrimitives[0][static_cast<size_t>(p)];
            const auto& want = plain.meshPrimitives[0][static_cast<size_t>(p)];
            const char* indexMode = p == 0 ? "TRIANGLES" : "INDICES";
            std::cout << "  primitive " << p << " (" << indexMode << " indices)\n";
            check(got.vertexCount == kVertices && got.vertexColors && got.vertexBytes.size() == want.vertexBytes.size(),
                "    every vertex, in the vertex-coloured layout");
            check(got.indexBytes == want.indexBytes, "    indices equal the plain file's, byte for byte");
            if (got.vertexCount != kVertices || got.vertexBytes.size() != want.vertexBytes.size()) {
                continue;
            }
            bool positions = true, uvs = true, normals = true, colors = true, asPlain = true;
            float worstNormal = 1.0f;
            for (int i = 0; i < kVertices; ++i) {
                const auto& a = vertexAt(got, i);
                const auto& b = vertexAt(want, i);
                const auto s = static_cast<size_t>(i);
                positions = positions && a.px == source.positions[s * 3] && a.py == source.positions[s * 3 + 1] &&
                    a.pz == source.positions[s * 3 + 2];
                uvs = uvs && std::abs(a.u - source.uvs[s * 2]) < 1e-6f && std::abs(a.v - source.uvs[s * 2 + 1]) < 1e-6f;
                // The parser keeps a quantised normal's length; compare its direction.
                const float nl = std::sqrt(a.nx * a.nx + a.ny * a.ny + a.nz * a.nz);
                const float nd = (a.nx * source.normals[s * 4] + a.ny * source.normals[s * 4 + 1] +
                    a.nz * source.normals[s * 4 + 2]) / nl;
                worstNormal = std::min(worstNormal, nd);
                normals = normals && nd > 0.9998f;
                const float* c = colorAt(got, i);
                for (int k = 0; k < 4; ++k) {
                    colors = colors && std::abs(c[k] - source.colors[s * 4 + static_cast<size_t>(k)]) <= 3.0f / 255.0f;
                }
                // Against the plain file: the same quantised values, so the same floats.
                asPlain = asPlain && std::memcmp(&a, &b, 8 * sizeof(float)) == 0 &&
                    std::memcmp(c, colorAt(want, i), 4 * sizeof(float)) == 0;
            }
            check(positions, "    ATTRIBUTES positions are the source floats exactly");
            check(uvs, "    EXPONENTIAL uvs match the source");
            if (!check(normals, "    OCTAHEDRAL normals within 1.2 degrees of the source")) {
                std::cout << "      worst cosine " << worstNormal << "\n";
            }
            check(colors, "    COLOR colours within 3/255 of the source");
            check(asPlain, "    position, normal, uv and colour equal the plain file's");
        }
    }
}

int main()
{
    std::cout << std::unitbuf;

    const Source source = makeSource();
    const auto streams = makeStreams(source);

    std::cout << "plain file\n";
    tinygltf::Model plainModel;
    if (!check(load(Kind::Plain, streams, plainModel), "  the plain file loads")) {
        return finish("glTF meshopt compression");
    }
    const auto plain = GlbParser::prepareFromModel(plainModel, PixelFormat::PIXELFORMAT_RGBA8, "plain");
    if (!check(plain.meshPrimitives.size() == 1 && plain.meshPrimitives[0].size() == 2, "  one mesh of two primitives")) {
        return finish("glTF meshopt compression");
    }

    {
        const auto [json, binary] = build(streams, Kind::CompressedGlb);
        size_t fallbackLength = 0;
        for (const auto& stream : streams) {
            fallbackLength += (stream.plain.size() + 3) & ~size_t{3};
        }
        check(fallbackLength > binary.size(), "the fallback buffer is larger than the GLB binary chunk");
    }

    compare("compressed GLB, fallback buffer without data", Kind::CompressedGlb, source, streams, plain);
    compare("compressed glTF JSON, fallback buffer naming a missing file", Kind::CompressedGltf, source, streams,
        plain);

    std::cout << "through parseFromMemory\n";
    {
        const auto [json, binary] = build(streams, Kind::CompressedGlb);
        const auto bytes = glb(json, binary);
        auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{.cpuBuffers = true});
        const auto container = GlbParser::parseFromMemory(bytes.data(), bytes.size(), device, "meshopt");
        check(container != nullptr, "  a compressed GLB loads into a container");
    }

    return finish("glTF meshopt compression");
}
