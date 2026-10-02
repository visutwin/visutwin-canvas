// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.07.2026
//
#include "gsplatData.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <istream>
#include <limits>
#include <optional>
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

        /// SH bands 1-3 per splat, coefficient-major interleaved RGB, zero-padded to 15
        /// coefficients — the layout the shader reads.
        using ShRow = std::array<float, 45>;

        // ── Header ───────────────────────────────────────────────────────────

        // Reads the generic ASCII header (multiple elements, mixed types) up to
        // end_header. Only binary little-endian files with fixed-size properties are
        // accepted; nullopt (logged) otherwise.
        std::optional<std::vector<PlyElement>> readPlyHeader(std::istream& file, const std::string& path)
        {
            std::string line;
            if (!std::getline(file, line) || line.rfind("ply", 0) != 0) {
                spdlog::error("GSplatData: '{}' is not a PLY file", path);
                return std::nullopt;
            }

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
                return std::nullopt;
            }
            return elements;
        }

        /// Reads a whole element's binary block; false when the file is short.
        bool readElementBlock(std::istream& file, const PlyElement& element, std::vector<uint8_t>& out)
        {
            out.resize(static_cast<size_t>(element.count) * element.stride);
            file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
            return static_cast<bool>(file);
        }

        // ── Uncompressed float PLY ───────────────────────────────────────────

        /// The vertex block of a standard 3DGS PLY and the column of each splat field
        /// in its rows (-1 when absent).
        struct UncompressedPly
        {
            int count = 0;
            size_t stride = 0;   // floats per row
            int px = -1, py = -1, pz = -1;
            int pr = -1, pg = -1, pb = -1;
            int po = -1;
            int ps0 = -1, ps1 = -1, ps2 = -1;
            int pq0 = -1, pq1 = -1, pq2 = -1, pq3 = -1;
            int shCoeffs = 0;           // per channel
            std::vector<int> fRest;     // columns of f_rest_0 .. f_rest_(3 * shCoeffs - 1)
            std::vector<uint8_t> block;
        };

        std::optional<UncompressedPly> readUncompressedPly(std::istream& file, const PlyElement& vtx,
            const std::string& path)
        {
            if (vtx.name != "vertex") {
                spdlog::error("GSplatData: '{}' first element is not 'vertex'", path);
                return std::nullopt;
            }
            std::unordered_map<std::string, int> propIndex;
            for (size_t i = 0; i < vtx.properties.size(); ++i) {
                if (vtx.properties[i].size != 4) {
                    spdlog::error("GSplatData: '{}' vertex property '{}' is not float",
                        path, vtx.properties[i].name);
                    return std::nullopt;
                }
                propIndex[vtx.properties[i].name] = static_cast<int>(i);
            }
            const auto prop = [&](const std::string& n) {
                const auto it = propIndex.find(n);
                return it != propIndex.end() ? it->second : -1;
            };

            UncompressedPly ply;
            ply.count = vtx.count;
            ply.stride = vtx.properties.size();
            ply.px = prop("x"); ply.py = prop("y"); ply.pz = prop("z");
            ply.pr = prop("f_dc_0"); ply.pg = prop("f_dc_1"); ply.pb = prop("f_dc_2");
            ply.po = prop("opacity");
            ply.ps0 = prop("scale_0"); ply.ps1 = prop("scale_1"); ply.ps2 = prop("scale_2");
            ply.pq0 = prop("rot_0"); ply.pq1 = prop("rot_1"); ply.pq2 = prop("rot_2"); ply.pq3 = prop("rot_3");
            if (ply.px < 0 || ply.py < 0 || ply.pz < 0 || ply.pr < 0 || ply.po < 0 || ply.ps0 < 0 || ply.pq0 < 0) {
                spdlog::error("GSplatData: '{}' is not a 3DGS PLY (missing splat properties)", path);
                return std::nullopt;
            }

            // SH bands from the count of present f_rest_* properties (9/24/45).
            while (true) {
                const int column = prop("f_rest_" + std::to_string(ply.fRest.size()));
                if (column < 0) {
                    break;
                }
                ply.fRest.push_back(column);
            }
            ply.shCoeffs = static_cast<int>(ply.fRest.size()) / 3;

            if (!readElementBlock(file, vtx, ply.block)) {
                spdlog::error("GSplatData: '{}' truncated vertex data", path);
                return std::nullopt;
            }
            return ply;
        }

        GpuSplat decodeUncompressedSplat(const float* row, const UncompressedPly& ply)
        {
            const float pos[3] = {row[ply.px], row[ply.py], row[ply.pz]};
            const float colorRGB[3] = {
                0.5f + row[ply.pr] * SH_C0,
                0.5f + (ply.pg >= 0 ? row[ply.pg] : row[ply.pr]) * SH_C0,
                0.5f + (ply.pb >= 0 ? row[ply.pb] : row[ply.pr]) * SH_C0
            };
            const float alpha = 1.0f / (1.0f + std::exp(-row[ply.po]));
            const float scale[3] = {
                std::exp(row[ply.ps0]),
                std::exp(ply.ps1 >= 0 ? row[ply.ps1] : row[ply.ps0]),
                std::exp(ply.ps2 >= 0 ? row[ply.ps2] : row[ply.ps0])
            };
            return buildSplat(pos, row[ply.pq0], row[ply.pq1], row[ply.pq2], row[ply.pq3], scale, colorRGB, alpha);
        }

        // f_rest is channel-major: [R(shCoeffs), G(shCoeffs), B(shCoeffs)].
        // Reorder to coefficient-major interleaved, zero-padded to 15.
        ShRow decodeUncompressedSh(const float* row, const UncompressedPly& ply)
        {
            ShRow sh{};
            const int n = ply.shCoeffs;
            for (int k = 0; k < n; ++k) {
                sh[static_cast<size_t>(k) * 3 + 0] = row[ply.fRest[static_cast<size_t>(0 * n + k)]];
                sh[static_cast<size_t>(k) * 3 + 1] = row[ply.fRest[static_cast<size_t>(1 * n + k)]];
                sh[static_cast<size_t>(k) * 3 + 2] = row[ply.fRest[static_cast<size_t>(2 * n + k)]];
            }
            return sh;
        }

        // ── Compressed SuperSplat PLY ────────────────────────────────────────

        /// elements: [0]=chunk (float), [1]=vertex (uint x4), [2]=sh (uchar, optional).
        /// Every 256 vertices share a chunk holding their min/max boxes.
        struct CompressedPly
        {
            int count = 0;
            int chunkSize = 0;   // floats per chunk: 12, or 18 with a colour box
            int shCoeffs = 0;    // per channel, 0 when there is no usable sh element
            std::vector<uint8_t> chunkBytes, vtxBytes, shBytes;
        };

        std::optional<CompressedPly> readCompressedPly(std::istream& file, const std::vector<PlyElement>& elements,
            const std::string& path)
        {
            if (elements.size() < 2 || elements[1].name != "vertex") {
                spdlog::error("GSplatData: '{}' malformed compressed PLY", path);
                return std::nullopt;
            }
            const PlyElement& chunkElem = elements[0];
            const PlyElement& vtxElem = elements[1];
            CompressedPly ply;
            ply.count = vtxElem.count;
            ply.chunkSize = static_cast<int>(chunkElem.properties.size());
            if ((ply.chunkSize != 12 && ply.chunkSize != 18) || vtxElem.properties.size() != 4) {
                spdlog::error("GSplatData: '{}' unexpected compressed layout (chunk={}, vtxProps={})",
                    path, ply.chunkSize, vtxElem.properties.size());
                return std::nullopt;
            }
            if (!readElementBlock(file, chunkElem, ply.chunkBytes) || !readElementBlock(file, vtxElem, ply.vtxBytes)) {
                spdlog::error("GSplatData: '{}' truncated compressed data", path);
                return std::nullopt;
            }
            if (elements.size() >= 3 && elements[2].name == "sh") {
                const int shCoeffs = static_cast<int>(elements[2].properties.size()) / 3;
                if (shBandsFromCoeffs(shCoeffs) == 0 || !readElementBlock(file, elements[2], ply.shBytes)) {
                    spdlog::warn("GSplatData: '{}' ignoring unrecognized 'sh' element", path);
                } else {
                    ply.shCoeffs = shCoeffs;
                }
            }
            return ply;
        }

        float unpackUnorm(const uint32_t value, const int bits)
        {
            const uint32_t t = (1u << bits) - 1u;
            return static_cast<float>(value & t) / static_cast<float>(t);
        }

        float lerp(const float a, const float b, const float t)
        {
            return a * (1.0f - t) + b * t;
        }

        GpuSplat decodeCompressedSplat(const CompressedPly& ply, const int i)
        {
            const auto* chunkData = reinterpret_cast<const float*>(ply.chunkBytes.data());
            const auto* vtxData = reinterpret_cast<const uint32_t*>(ply.vtxBytes.data());
            const int ci = (i / 256) * ply.chunkSize;
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
            if (ply.chunkSize > 12) {
                colorRGB[0] = lerp(chunkData[ci + 12], chunkData[ci + 15], colorRGB[0]);
                colorRGB[1] = lerp(chunkData[ci + 13], chunkData[ci + 16], colorRGB[1]);
                colorRGB[2] = lerp(chunkData[ci + 14], chunkData[ci + 17], colorRGB[2]);
            }
            const float alpha = unpackUnorm(pColor, 8);

            return buildSplat(pos, qw, qx, qy, qz, scale, colorRGB, alpha);
        }

        // The sh element is channel-major uchar: [R(shCoeffs), G, B]; dequant
        // val = u8 * (8/255) - 4, reorder to coefficient-major interleaved.
        ShRow decodeCompressedSh(const CompressedPly& ply, const int i)
        {
            const uint8_t* shRow = ply.shBytes.data() + static_cast<size_t>(i) * ply.shCoeffs * 3;
            ShRow sh{};
            for (int k = 0; k < ply.shCoeffs; ++k) {
                for (int c = 0; c < 3; ++c) {
                    const uint8_t q = shRow[static_cast<size_t>(c) * ply.shCoeffs + k];
                    sh[static_cast<size_t>(k) * 3 + c] = static_cast<float>(q) * (8.0f / 255.0f) - 4.0f;
                }
            }
            return sh;
        }
    }

    std::unique_ptr<GSplatData> GSplatData::loadPly(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            spdlog::error("GSplatData: cannot open '{}'", path);
            return nullptr;
        }
        const auto elements = readPlyHeader(file, path);
        if (!elements) {
            return nullptr;
        }
        const bool compressed = elements->front().name == "chunk";

        auto data = std::make_unique<GSplatData>();
        BoundsAccumulator bounds;
        if (compressed) {
            const auto ply = readCompressedPly(file, *elements, path);
            if (!ply) {
                return nullptr;
            }
            data->reserveSplats(static_cast<size_t>(ply->count), shBandsFromCoeffs(ply->shCoeffs));
            for (int i = 0; i < ply->count; ++i) {
                data->appendSplat(decodeCompressedSplat(*ply, i), bounds);
                if (data->_shBands > 0) {
                    data->appendShRow(decodeCompressedSh(*ply, i).data());
                }
            }
        } else {
            const auto ply = readUncompressedPly(file, elements->front(), path);
            if (!ply) {
                return nullptr;
            }
            data->reserveSplats(static_cast<size_t>(ply->count), shBandsFromCoeffs(ply->shCoeffs));
            const auto* rows = reinterpret_cast<const float*>(ply->block.data());
            for (int i = 0; i < ply->count; ++i) {
                const float* row = rows + static_cast<size_t>(i) * ply->stride;
                if (!std::isfinite(row[ply->px]) || !std::isfinite(row[ply->py]) || !std::isfinite(row[ply->pz])) {
                    continue;
                }
                data->appendSplat(decodeUncompressedSplat(row, *ply), bounds);
                if (data->_shBands > 0) {
                    data->appendShRow(decodeUncompressedSh(row, *ply).data());
                }
            }
        }

        if (data->_splats.empty()) {
            spdlog::error("GSplatData: '{}' contains no valid splats", path);
            return nullptr;
        }

        data->finishBounds(bounds, path);

        spdlog::info("GSplatData: loaded '{}' — {} splats ({}, SH bands {})",
            path, data->_splats.size(), compressed ? "compressed" : "uncompressed", data->_shBands);
        return data;
    }

    void GSplatData::reserveSplats(const size_t count, const int shBands)
    {
        _shBands = shBands;
        _splats.reserve(count);
        _centers.reserve(count * 3);
        if (shBands > 0) {
            _shCoeffs.reserve(count * 45);
        }
    }

    void GSplatData::appendShRow(const float* sh45)
    {
        _shCoeffs.insert(_shCoeffs.end(), sh45, sh45 + 45);
    }

    void GSplatData::appendSplat(const GpuSplat& s, BoundsAccumulator& bounds)
    {
        _centers.push_back(s.center[0]);
        _centers.push_back(s.center[1]);
        _centers.push_back(s.center[2]);
        _splats.push_back(s);

        // The bounds have to carry each splat's EXTENT, not just its centre. A
        // splat is an ellipsoid, so the cloud reaches visibly past the hull of
        // its centres — by the size of the largest splat on the rim, which in a
        // capture is usually one of the big background ones. Bounding the centres
        // alone culls the whole cloud a moment early as the camera turns away, and
        // the entire thing blinks out while part of it is still on screen.
        //
        // DEVIATION from upstream in the tighter direction, because this port
        // stores something upstream does not. `calcAabb` pads isotropically by
        // twice the LARGEST of the three scales and `calcAabbExact` takes the box
        // of the rotated 2-sigma box; both are bounds of the ellipsoid rather than
        // the ellipsoid's own. This parser discards rotation and scale at load and
        // keeps the composed covariance Sigma = R S^2 R^T, whose DIAGONAL is the
        // variance along each model axis — so sqrt of it is exactly the standard
        // deviation along that axis and 2 sigma is the tightest axis-aligned bound
        // there is. Same 2-sigma convention as upstream, so the two agree on a
        // sphere and this one is smaller on anything elongated or rotated.
        //
        // max(., 0) because a variance is non-negative by construction but a
        // near-degenerate splat can land a tiny negative there through rounding,
        // and sqrt of that is a NaN which would swallow the whole bound.
        const float extentX = 2.0f * std::sqrt(std::max(s.covA[0], 0.0f));
        const float extentY = 2.0f * std::sqrt(std::max(s.covB[0], 0.0f));
        const float extentZ = 2.0f * std::sqrt(std::max(s.covB[2], 0.0f));

        // Skip a non-finite splat rather than let it poison the bounds. The splat itself is kept — it is the renderer's business, and one
        // bad record should not move the box every other splat is culled by.
        if (!std::isfinite(s.center[0]) || !std::isfinite(s.center[1]) ||
            !std::isfinite(s.center[2]) || !std::isfinite(extentX) ||
            !std::isfinite(extentY) || !std::isfinite(extentZ)) {
            return;
        }

        const Vector3 center = Vector3::load(s.center);
        const Vector3 extent(extentX, extentY, extentZ);
        bounds.min = Vector3::min(bounds.min, center - extent);
        bounds.max = Vector3::max(bounds.max, center + extent);
        bounds.set = true;
    }

    void GSplatData::finishBounds(const BoundsAccumulator& bounds, const std::string& source)
    {
        if (bounds.set) {
            _aabb.setCenter((bounds.min + bounds.max) * 0.5f);
            _aabb.setHalfExtents((bounds.max - bounds.min) * 0.5f);
        } else {
            // Every splat was non-finite. The sentinels would make a box with a
            // negative size that culls unpredictably, so say so and leave the
            // degenerate box the default gives.
            spdlog::warn("GSplatData: '{}' has no finite splat bounds", source);
            _aabb.setCenter(Vector3(0.0f));
            _aabb.setHalfExtents(Vector3(0.0f));
        }
    }

    std::unique_ptr<GSplatData> GSplatData::fromActivated(const ActivatedSplats& in, const std::string& source)
    {
        const size_t count = in.count;
        if (count == 0 || in.positions.size() < count * 3 || in.rotations.size() < count * 4 ||
            in.scales.size() < count * 3 || in.opacities.size() < count || in.sh0.size() < count * 3) {
            spdlog::error("GSplatData: '{}' splat attributes are missing or short", source);
            return nullptr;
        }
        const int bands = std::clamp(in.shBands, 0, 3);
        static constexpr int kCoeffsForBands[] = {0, 3, 8, 15};
        const int coeffs = kCoeffsForBands[bands];
        if (bands > 0 && in.shRest.size() < count * static_cast<size_t>(coeffs) * 3) {
            spdlog::error("GSplatData: '{}' SH coefficients are short", source);
            return nullptr;
        }

        auto data = std::make_unique<GSplatData>();
        data->reserveSplats(count, bands);
        BoundsAccumulator bounds;
        for (size_t i = 0; i < count; ++i) {
            const float* pos = in.positions.data() + i * 3;
            if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) {
                continue;
            }
            // The same packing as the PLY path, but the values arrive ACTIVATED: the
            // scale is linear (no exp) and the opacity already went through the
            // sigmoid. The colour is the SH degree-0 coefficient, decoded as the PLY's
            // f_dc is. glTF quaternions are xyzw.
            const float* dc = in.sh0.data() + i * 3;
            const float colorRGB[3] = {0.5f + dc[0] * SH_C0, 0.5f + dc[1] * SH_C0, 0.5f + dc[2] * SH_C0};
            const float* q = in.rotations.data() + i * 4;
            data->appendSplat(buildSplat(pos, q[3], q[0], q[1], q[2], in.scales.data() + i * 3, colorRGB,
                in.opacities[i]), bounds);
            if (bands > 0) {
                // Already coefficient-major interleaved; pad to the 15 the shader reads.
                ShRow sh{};
                std::copy_n(in.shRest.data() + i * static_cast<size_t>(coeffs) * 3, coeffs * 3, sh.begin());
                data->appendShRow(sh.data());
            }
        }
        if (data->_splats.empty()) {
            spdlog::error("GSplatData: '{}' contains no valid splats", source);
            return nullptr;
        }
        data->finishBounds(bounds, source);
        return data;
    }
}
