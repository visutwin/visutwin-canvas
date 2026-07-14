// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "gsplatData.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace visutwin::canvas
{
    namespace
    {
        constexpr float SH_C0 = 0.28209479177387814f;

        uint8_t toByte(const float v)
        {
            return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        }

        // ── Generic PLY header model ─────────────────────────────────────────
        struct PlyProperty
        {
            std::string name;
            int size = 0;   // bytes (float/uint = 4, uchar = 1, ...)
        };

        struct PlyElement
        {
            std::string name;
            int count = 0;
            std::vector<PlyProperty> properties;
            size_t stride = 0;   // bytes per row
        };

        int propertyTypeSize(const std::string& type)
        {
            if (type == "float" || type == "float32" || type == "int" ||
                type == "int32" || type == "uint" || type == "uint32") {
                return 4;
            }
            if (type == "double" || type == "float64") return 8;
            if (type == "short" || type == "ushort" || type == "int16" || type == "uint16") return 2;
            if (type == "char" || type == "uchar" || type == "int8" || type == "uint8") return 1;
            return 0;   // unsupported (e.g. list)
        }

        // Build Sigma = R * S^2 * R^T from a normalized quaternion (w,x,y,z) and
        // world-space scale, pack the display color + opacity, and produce a
        // GpuSplat. colorRGB is display-linear-ish [0,1]; alpha is [0,1].
        GpuSplat buildSplat(const float pos[3], float qw, float qx, float qy, float qz,
            const float scale[3], const float colorRGB[3], float alpha)
        {
            GpuSplat splat{};
            splat.center[0] = pos[0];
            splat.center[1] = pos[1];
            splat.center[2] = pos[2];

            splat.color =
                (static_cast<uint32_t>(toByte(colorRGB[0]))) |
                (static_cast<uint32_t>(toByte(colorRGB[1])) << 8) |
                (static_cast<uint32_t>(toByte(colorRGB[2])) << 16) |
                (static_cast<uint32_t>(toByte(alpha)) << 24);

            const float qlen = std::sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
            if (qlen > 1e-8f) {
                qw /= qlen; qx /= qlen; qy /= qlen; qz /= qlen;
            } else {
                qw = 1.0f; qx = qy = qz = 0.0f;
            }
            const float r[3][3] = {
                {1.0f - 2.0f * (qy * qy + qz * qz), 2.0f * (qx * qy - qw * qz), 2.0f * (qx * qz + qw * qy)},
                {2.0f * (qx * qy + qw * qz), 1.0f - 2.0f * (qx * qx + qz * qz), 2.0f * (qy * qz - qw * qx)},
                {2.0f * (qx * qz - qw * qy), 2.0f * (qy * qz + qw * qx), 1.0f - 2.0f * (qx * qx + qy * qy)}
            };
            const float s2[3] = {scale[0] * scale[0], scale[1] * scale[1], scale[2] * scale[2]};
            const auto sigma = [&](const int a, const int b) {
                return s2[0] * r[a][0] * r[b][0] + s2[1] * r[a][1] * r[b][1] + s2[2] * r[a][2] * r[b][2];
            };
            splat.covA[0] = sigma(0, 0);
            splat.covA[1] = sigma(0, 1);
            splat.covA[2] = sigma(0, 2);
            splat.covB[0] = sigma(1, 1);
            splat.covB[1] = sigma(1, 2);
            splat.covB[2] = sigma(2, 2);
            return splat;
        }

        int shBandsFromCoeffs(const int coeffsPerChannel)
        {
            switch (coeffsPerChannel) {
                case 3:  return 1;
                case 8:  return 2;
                case 15: return 3;
                default: return 0;
            }
        }
    }

    std::unique_ptr<GSplatData> GSplatData::loadPly(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            spdlog::error("GSplatData: cannot open '{}'", path);
            return nullptr;
        }

        std::string line;
        if (!std::getline(file, line) || line.rfind("ply", 0) != 0) {
            spdlog::error("GSplatData: '{}' is not a PLY file", path);
            return nullptr;
        }

        // ── Parse the generic ASCII header (multiple elements, mixed types) ──
        bool binaryLittleEndian = false;
        std::vector<PlyElement> elements;
        bool unsupportedProperty = false;

        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == "end_header") break;

            std::istringstream tokens(line);
            std::string keyword;
            tokens >> keyword;
            if (keyword == "format") {
                std::string format;
                tokens >> format;
                binaryLittleEndian = (format == "binary_little_endian");
            } else if (keyword == "element") {
                PlyElement element;
                tokens >> element.name >> element.count;
                elements.push_back(std::move(element));
            } else if (keyword == "property" && !elements.empty()) {
                std::string type, name;
                tokens >> type >> name;
                if (type == "list") {
                    unsupportedProperty = true;   // PLY lists are not supported
                    continue;
                }
                const int size = propertyTypeSize(type);
                if (size == 0) {
                    unsupportedProperty = true;
                }
                elements.back().properties.push_back({name, size});
                elements.back().stride += static_cast<size_t>(size);
            }
        }

        if (!binaryLittleEndian || elements.empty() || unsupportedProperty) {
            spdlog::error("GSplatData: '{}' unsupported PLY header", path);
            return nullptr;
        }

        const bool compressed = (elements.front().name == "chunk");

        auto data = std::make_unique<GSplatData>();
        Vector3 minPos(std::numeric_limits<float>::max());
        Vector3 maxPos(std::numeric_limits<float>::lowest());
        const auto accumulate = [&](const GpuSplat& s) {
            data->_centers.push_back(s.center[0]);
            data->_centers.push_back(s.center[1]);
            data->_centers.push_back(s.center[2]);
            data->_splats.push_back(s);
            minPos = Vector3(std::min(minPos.getX(), s.center[0]),
                             std::min(minPos.getY(), s.center[1]),
                             std::min(minPos.getZ(), s.center[2]));
            maxPos = Vector3(std::max(maxPos.getX(), s.center[0]),
                             std::max(maxPos.getY(), s.center[1]),
                             std::max(maxPos.getZ(), s.center[2]));
        };

        // Read a whole element's binary block into a byte buffer.
        const auto readBlock = [&](const PlyElement& e, std::vector<uint8_t>& out) -> bool {
            out.resize(static_cast<size_t>(e.count) * e.stride);
            file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
            return static_cast<bool>(file);
        };

        if (!compressed) {
            // ── Uncompressed float PLY ───────────────────────────────────────
            const PlyElement& vtx = elements.front();
            if (vtx.name != "vertex") {
                spdlog::error("GSplatData: '{}' first element is not 'vertex'", path);
                return nullptr;
            }
            std::unordered_map<std::string, int> propIndex;
            for (size_t i = 0; i < vtx.properties.size(); ++i) {
                if (vtx.properties[i].size != 4) {
                    spdlog::error("GSplatData: '{}' vertex property '{}' is not float",
                        path, vtx.properties[i].name);
                    return nullptr;
                }
                propIndex[vtx.properties[i].name] = static_cast<int>(i);
            }
            const auto prop = [&](const char* n) {
                const auto it = propIndex.find(n);
                return it != propIndex.end() ? it->second : -1;
            };

            const int px = prop("x"), py = prop("y"), pz = prop("z");
            const int pr = prop("f_dc_0"), pg = prop("f_dc_1"), pb = prop("f_dc_2");
            const int po = prop("opacity");
            const int ps0 = prop("scale_0"), ps1 = prop("scale_1"), ps2 = prop("scale_2");
            const int pq0 = prop("rot_0"), pq1 = prop("rot_1"), pq2 = prop("rot_2"), pq3 = prop("rot_3");
            if (px < 0 || py < 0 || pz < 0 || pr < 0 || po < 0 || ps0 < 0 || pq0 < 0) {
                spdlog::error("GSplatData: '{}' is not a 3DGS PLY (missing splat properties)", path);
                return nullptr;
            }

            // SH bands from the count of present f_rest_* properties (9/24/45).
            int fRestCount = 0;
            while (prop(("f_rest_" + std::to_string(fRestCount)).c_str()) >= 0) {
                ++fRestCount;
            }
            const int shCoeffs = fRestCount / 3;   // per channel
            data->_shBands = shBandsFromCoeffs(shCoeffs);
            std::vector<int> fRest(static_cast<size_t>(fRestCount));
            for (int i = 0; i < fRestCount; ++i) {
                fRest[static_cast<size_t>(i)] = prop(("f_rest_" + std::to_string(i)).c_str());
            }

            std::vector<uint8_t> block;
            if (!readBlock(vtx, block)) {
                spdlog::error("GSplatData: '{}' truncated vertex data", path);
                return nullptr;
            }
            const auto* rows = reinterpret_cast<const float*>(block.data());
            const size_t stride = vtx.properties.size();

            data->_splats.reserve(static_cast<size_t>(vtx.count));
            data->_centers.reserve(static_cast<size_t>(vtx.count) * 3);
            if (data->_shBands > 0) {
                data->_shCoeffs.reserve(static_cast<size_t>(vtx.count) * 45);
            }

            for (int i = 0; i < vtx.count; ++i) {
                const float* row = rows + static_cast<size_t>(i) * stride;
                const float pos[3] = {row[px], row[py], row[pz]};
                if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) {
                    continue;
                }
                const float colorRGB[3] = {
                    0.5f + row[pr] * SH_C0,
                    0.5f + (pg >= 0 ? row[pg] : row[pr]) * SH_C0,
                    0.5f + (pb >= 0 ? row[pb] : row[pr]) * SH_C0
                };
                const float alpha = 1.0f / (1.0f + std::exp(-row[po]));
                const float scale[3] = {
                    std::exp(row[ps0]),
                    std::exp(ps1 >= 0 ? row[ps1] : row[ps0]),
                    std::exp(ps2 >= 0 ? row[ps2] : row[ps0])
                };
                accumulate(buildSplat(pos, row[pq0], row[pq1], row[pq2], row[pq3], scale, colorRGB, alpha));

                if (data->_shBands > 0) {
                    // f_rest is channel-major: [R(shCoeffs), G(shCoeffs), B(shCoeffs)].
                    // Reorder to coefficient-major interleaved, zero-padded to 15.
                    std::array<float, 45> sh{};
                    for (int k = 0; k < shCoeffs; ++k) {
                        sh[static_cast<size_t>(k) * 3 + 0] = row[fRest[static_cast<size_t>(0 * shCoeffs + k)]];
                        sh[static_cast<size_t>(k) * 3 + 1] = row[fRest[static_cast<size_t>(1 * shCoeffs + k)]];
                        sh[static_cast<size_t>(k) * 3 + 2] = row[fRest[static_cast<size_t>(2 * shCoeffs + k)]];
                    }
                    data->_shCoeffs.insert(data->_shCoeffs.end(), sh.begin(), sh.end());
                }
            }
        } else {
            // ── Compressed SuperSplat PLY ────────────────────────────────────
            // elements: [0]=chunk (float), [1]=vertex (uint x4), [2]=sh (uchar, optional)
            if (elements.size() < 2 || elements[1].name != "vertex") {
                spdlog::error("GSplatData: '{}' malformed compressed PLY", path);
                return nullptr;
            }
            const PlyElement& chunkElem = elements[0];
            const PlyElement& vtxElem = elements[1];
            const int chunkSize = static_cast<int>(chunkElem.properties.size());   // 12 or 18
            if ((chunkSize != 12 && chunkSize != 18) || vtxElem.properties.size() != 4) {
                spdlog::error("GSplatData: '{}' unexpected compressed layout (chunk={}, vtxProps={})",
                    path, chunkSize, vtxElem.properties.size());
                return nullptr;
            }

            std::vector<uint8_t> chunkBytes, vtxBytes, shBytes;
            if (!readBlock(chunkElem, chunkBytes) || !readBlock(vtxElem, vtxBytes)) {
                spdlog::error("GSplatData: '{}' truncated compressed data", path);
                return nullptr;
            }
            const auto* chunkData = reinterpret_cast<const float*>(chunkBytes.data());
            const auto* vtxData = reinterpret_cast<const uint32_t*>(vtxBytes.data());

            int shCoeffs = 0;
            if (elements.size() >= 3 && elements[2].name == "sh") {
                shCoeffs = static_cast<int>(elements[2].properties.size()) / 3;
                data->_shBands = shBandsFromCoeffs(shCoeffs);
                if (data->_shBands == 0 || !readBlock(elements[2], shBytes)) {
                    spdlog::warn("GSplatData: '{}' ignoring unrecognized 'sh' element", path);
                    data->_shBands = 0;
                    shCoeffs = 0;
                }
            }
            const int shStride = shCoeffs * 3;   // uchar per splat

            const auto unpackUnorm = [](const uint32_t value, const int bits) {
                const uint32_t t = (1u << bits) - 1u;
                return static_cast<float>(value & t) / static_cast<float>(t);
            };
            const auto lerp = [](const float a, const float b, const float t) { return a * (1.0f - t) + b * t; };

            data->_splats.reserve(static_cast<size_t>(vtxElem.count));
            data->_centers.reserve(static_cast<size_t>(vtxElem.count) * 3);
            if (data->_shBands > 0) {
                data->_shCoeffs.reserve(static_cast<size_t>(vtxElem.count) * 45);
            }

            for (int i = 0; i < vtxElem.count; ++i) {
                const int ci = (i / 256) * chunkSize;
                const uint32_t pPos = vtxData[static_cast<size_t>(i) * 4 + 0];
                const uint32_t pRot = vtxData[static_cast<size_t>(i) * 4 + 1];
                const uint32_t pScale = vtxData[static_cast<size_t>(i) * 4 + 2];
                const uint32_t pColor = vtxData[static_cast<size_t>(i) * 4 + 3];

                // Position: 11-10-11 unorm lerped into the chunk's min/max box.
                const float pos[3] = {
                    lerp(chunkData[ci + 0], chunkData[ci + 3], unpackUnorm(pPos >> 21, 11)),
                    lerp(chunkData[ci + 1], chunkData[ci + 4], unpackUnorm(pPos >> 11, 10)),
                    lerp(chunkData[ci + 2], chunkData[ci + 5], unpackUnorm(pPos, 11))
                };

                // Rotation: 2-bit largest-index + 3x10-bit remaining, scaled by sqrt(2).
                const float norm = 1.41421356237f;
                const float ra = (unpackUnorm(pRot >> 20, 10) - 0.5f) * norm;
                const float rb = (unpackUnorm(pRot >> 10, 10) - 0.5f) * norm;
                const float rc = (unpackUnorm(pRot, 10) - 0.5f) * norm;
                const float rm = std::sqrt(std::max(0.0f, 1.0f - (ra * ra + rb * rb + rc * rc)));
                float qx, qy, qz, qw;
                switch (pRot >> 30) {
                    case 0:  qx = ra; qy = rb; qz = rc; qw = rm; break;
                    case 1:  qx = rm; qy = rb; qz = rc; qw = ra; break;
                    case 2:  qx = rb; qy = rm; qz = rc; qw = ra; break;
                    default: qx = rb; qy = rc; qz = rm; qw = ra; break;
                }

                // Scale: 11-10-11 unorm lerped into log-space chunk min/max, then exp.
                const float scale[3] = {
                    std::exp(lerp(chunkData[ci + 6], chunkData[ci + 9], unpackUnorm(pScale >> 21, 11))),
                    std::exp(lerp(chunkData[ci + 7], chunkData[ci + 10], unpackUnorm(pScale >> 11, 10))),
                    std::exp(lerp(chunkData[ci + 8], chunkData[ci + 11], unpackUnorm(pScale, 11)))
                };

                // Color: 8888 unorm; rgb lerped into the chunk color box when present.
                float colorRGB[3] = {
                    unpackUnorm(pColor >> 24, 8),
                    unpackUnorm(pColor >> 16, 8),
                    unpackUnorm(pColor >> 8, 8)
                };
                if (chunkSize > 12) {
                    colorRGB[0] = lerp(chunkData[ci + 12], chunkData[ci + 15], colorRGB[0]);
                    colorRGB[1] = lerp(chunkData[ci + 13], chunkData[ci + 16], colorRGB[1]);
                    colorRGB[2] = lerp(chunkData[ci + 14], chunkData[ci + 17], colorRGB[2]);
                }
                const float alpha = unpackUnorm(pColor, 8);

                accumulate(buildSplat(pos, qw, qx, qy, qz, scale, colorRGB, alpha));

                if (data->_shBands > 0) {
                    // sh element is channel-major uchar: [R(shCoeffs), G, B]; dequant
                    // val = u8 * (8/255) - 4, reorder to coefficient-major interleaved.
                    const uint8_t* shRow = shBytes.data() + static_cast<size_t>(i) * shStride;
                    std::array<float, 45> sh{};
                    for (int k = 0; k < shCoeffs; ++k) {
                        for (int c = 0; c < 3; ++c) {
                            const uint8_t q = shRow[static_cast<size_t>(c) * shCoeffs + k];
                            sh[static_cast<size_t>(k) * 3 + c] = static_cast<float>(q) * (8.0f / 255.0f) - 4.0f;
                        }
                    }
                    data->_shCoeffs.insert(data->_shCoeffs.end(), sh.begin(), sh.end());
                }
            }
        }

        if (data->_splats.empty()) {
            spdlog::error("GSplatData: '{}' contains no valid splats", path);
            return nullptr;
        }

        data->_aabb.setCenter((minPos + maxPos) * 0.5f);
        data->_aabb.setHalfExtents((maxPos - minPos) * 0.5f);

        spdlog::info("GSplatData: loaded '{}' — {} splats ({}, SH bands {})",
            path, data->_splats.size(), compressed ? "compressed" : "uncompressed", data->_shBands);
        return data;
    }
}
