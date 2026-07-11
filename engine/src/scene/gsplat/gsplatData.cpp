// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "gsplatData.h"

#include <algorithm>
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
    }

    std::unique_ptr<GSplatData> GSplatData::loadPly(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            spdlog::error("GSplatData: cannot open '{}'", path);
            return nullptr;
        }

        // ── Parse the ASCII header ───────────────────────────────────────
        std::string line;
        if (!std::getline(file, line) || line.rfind("ply", 0) != 0) {
            spdlog::error("GSplatData: '{}' is not a PLY file", path);
            return nullptr;
        }

        int vertexCount = 0;
        bool binaryLittleEndian = false;
        std::vector<std::string> properties;   // in declaration order (all must be float)
        bool inVertexElement = false;
        bool badPropertyType = false;

        while (std::getline(file, line)) {
            // Tolerate \r\n line endings.
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
                std::string elementName;
                tokens >> elementName >> vertexCount;
                inVertexElement = (elementName == "vertex");
            } else if (keyword == "property" && inVertexElement) {
                std::string type, name;
                tokens >> type >> name;
                if (type != "float" && type != "float32") {
                    badPropertyType = true;
                }
                properties.push_back(name);
            }
        }

        if (!binaryLittleEndian || vertexCount <= 0 || badPropertyType) {
            spdlog::error("GSplatData: '{}' unsupported PLY (need binary_little_endian float "
                "vertex properties; count={})", path, vertexCount);
            return nullptr;
        }

        std::unordered_map<std::string, int> propertyIndex;
        for (size_t i = 0; i < properties.size(); ++i) {
            propertyIndex[properties[i]] = static_cast<int>(i);
        }
        const auto prop = [&](const char* name) {
            const auto it = propertyIndex.find(name);
            return it != propertyIndex.end() ? it->second : -1;
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

        // ── Read the binary body ─────────────────────────────────────────
        const size_t stride = properties.size();
        std::vector<float> row(stride);

        auto data = std::make_unique<GSplatData>();
        data->_splats.reserve(static_cast<size_t>(vertexCount));
        data->_centers.reserve(static_cast<size_t>(vertexCount) * 3);

        Vector3 minPos(std::numeric_limits<float>::max());
        Vector3 maxPos(std::numeric_limits<float>::lowest());

        for (int i = 0; i < vertexCount; ++i) {
            file.read(reinterpret_cast<char*>(row.data()),
                static_cast<std::streamsize>(stride * sizeof(float)));
            if (!file) {
                spdlog::error("GSplatData: '{}' truncated at splat {}/{}", path, i, vertexCount);
                return nullptr;
            }

            GpuSplat splat{};
            splat.center[0] = row[px];
            splat.center[1] = row[py];
            splat.center[2] = row[pz];
            if (!std::isfinite(splat.center[0]) || !std::isfinite(splat.center[1]) ||
                !std::isfinite(splat.center[2])) {
                continue;
            }

            // Color: SH0 DC term → display color, opacity through sigmoid (PLY convention).
            const float alpha = 1.0f / (1.0f + std::exp(-row[po]));
            splat.color =
                (static_cast<uint32_t>(toByte(0.5f + row[pr] * SH_C0))) |
                (static_cast<uint32_t>(toByte(0.5f + (pg >= 0 ? row[pg] : row[pr]) * SH_C0)) << 8) |
                (static_cast<uint32_t>(toByte(0.5f + (pb >= 0 ? row[pb] : row[pr]) * SH_C0)) << 16) |
                (static_cast<uint32_t>(toByte(alpha)) << 24);

            // Scale is log-space in PLY.
            const float sx = std::exp(row[ps0]);
            const float sy = std::exp(ps1 >= 0 ? row[ps1] : row[ps0]);
            const float sz = std::exp(ps2 >= 0 ? row[ps2] : row[ps0]);

            // Rotation quaternion stored as (w, x, y, z); normalize.
            float qw = row[pq0], qx = row[pq1], qy = row[pq2], qz = row[pq3];
            const float qlen = std::sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
            if (qlen > 1e-8f) {
                qw /= qlen; qx /= qlen; qy /= qlen; qz /= qlen;
            } else {
                qw = 1.0f; qx = qy = qz = 0.0f;
            }

            // Rotation matrix rows (standard quaternion → matrix).
            const float r[3][3] = {
                {1.0f - 2.0f * (qy * qy + qz * qz), 2.0f * (qx * qy - qw * qz), 2.0f * (qx * qz + qw * qy)},
                {2.0f * (qx * qy + qw * qz), 1.0f - 2.0f * (qx * qx + qz * qz), 2.0f * (qy * qz - qw * qx)},
                {2.0f * (qx * qz - qw * qy), 2.0f * (qy * qz + qw * qx), 1.0f - 2.0f * (qx * qx + qy * qy)}
            };
            const float s2[3] = {sx * sx, sy * sy, sz * sz};

            // Sigma = R * S^2 * R^T (upper triangular).
            const auto sigma = [&](const int a, const int b) {
                return s2[0] * r[a][0] * r[b][0] + s2[1] * r[a][1] * r[b][1] + s2[2] * r[a][2] * r[b][2];
            };
            splat.covA[0] = sigma(0, 0);
            splat.covA[1] = sigma(0, 1);
            splat.covA[2] = sigma(0, 2);
            splat.covB[0] = sigma(1, 1);
            splat.covB[1] = sigma(1, 2);
            splat.covB[2] = sigma(2, 2);

            data->_centers.push_back(splat.center[0]);
            data->_centers.push_back(splat.center[1]);
            data->_centers.push_back(splat.center[2]);
            data->_splats.push_back(splat);

            minPos = Vector3(std::min(minPos.getX(), splat.center[0]),
                             std::min(minPos.getY(), splat.center[1]),
                             std::min(minPos.getZ(), splat.center[2]));
            maxPos = Vector3(std::max(maxPos.getX(), splat.center[0]),
                             std::max(maxPos.getY(), splat.center[1]),
                             std::max(maxPos.getZ(), splat.center[2]));
        }

        if (data->_splats.empty()) {
            spdlog::error("GSplatData: '{}' contains no valid splats", path);
            return nullptr;
        }

        data->_aabb.setCenter((minPos + maxPos) * 0.5f);
        data->_aabb.setHalfExtents((maxPos - minPos) * 0.5f);

        spdlog::info("GSplatData: loaded '{}' — {} splats ({} properties/vertex)",
            path, data->_splats.size(), stride);
        return data;
    }
}
