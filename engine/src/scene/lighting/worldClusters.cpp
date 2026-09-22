// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//

#include "worldClusters.h"
#include "lightTextureAtlas.h"

#include <limits>

#include "lightBounds.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    WorldClusters::WorldClusters(const ClusterConfig& config)
        : _config(config)
    {
        const size_t cellCount = static_cast<size_t>(_config.totalCells());
        const size_t cellDataSize = cellCount * static_cast<size_t>(_config.maxLightsPerCell);
        _cellData.resize(cellDataSize, 0u);
    }

    Vector3 WorldClusters::cellsCountByBoundsSize() const
    {
        const auto range = boundsRange();
        const float rx = range.getX() > 1e-6f ? static_cast<float>(_config.cellsX) / range.getX() : 0.0f;
        const float ry = range.getY() > 1e-6f ? static_cast<float>(_config.cellsY) / range.getY() : 0.0f;
        const float rz = range.getZ() > 1e-6f ? static_cast<float>(_config.cellsZ) / range.getZ() : 0.0f;
        return {rx, ry, rz};
    }

    void WorldClusters::update(const std::vector<ClusterLightData>& localLights)
    {
        _warnedOverflow = false;

        collectLights(localLights);
        computeGridBounds();
        assignLightsToCells();
        packGpuLights();
    }

    void WorldClusters::collectLights(const std::vector<ClusterLightData>& localLights)
    {
        _lights.clear();
        _lights.reserve(std::min(localLights.size(), static_cast<size_t>(255)));

        const auto toRadians = [](const float degrees) {
            return degrees * (std::numbers::pi_v<float> / 180.0f);
        };

        for (size_t i = 0; i < localLights.size() && _lights.size() < 255; ++i) {
            const auto& ld = localLights[i];

            if (ld.intensity <= 0.0f || ld.range <= 0.0f) {
                continue;
            }

            LightEntry entry;
            entry.data = ld;
            // Half-angles in degrees, as everywhere else (upstream convention).
            entry.outerConeCos = std::cos(toRadians(std::max(ld.outerConeAngle, 0.0f)));
            entry.innerConeCos = std::cos(toRadians(std::max(ld.innerConeAngle, 0.0f)));
            if (entry.innerConeCos < entry.outerConeCos) {
                entry.innerConeCos = entry.outerConeCos;
            }

            // World-space bound of what the light can actually reach. A spot gets its
            // CONE, not its range sphere: the sphere was about thirty times the
            // volume at a 20-degree cone, and since the grid is sized from the union
            // of these bounds, that slack coarsened every cell in the scene.
            entry.aabb = ld.isSpot
                ? spotConeAabb(ld.position, ld.direction, ld.range, ld.outerConeAngle)
                : omniAabb(ld.position, ld.range);

            _lights.push_back(entry);
        }
    }

    void WorldClusters::computeGridBounds()
    {
        if (_lights.empty()) {
            // Any small volume. Nothing samples it — the renderer only binds the grid
            // when it holds lights — and a degenerate one would divide by zero below.
            _boundsMin = Vector3(0.0f);
            _boundsMax = Vector3(1.0f);
        } else {
            // The union of the LIGHT bounds and nothing else, as upstream's
            // evaluateBounds does. This used to start from the camera's position
            // padded by 50 units in every direction, so the grid was a 100-unit cube
            // wherever the lights actually were: the cells came out coarse, most of
            // them empty, and a scene whose lights sat in one corner spent its whole
            // cell budget on space no light could reach.
            //
            // Shrinking the grid to the lights does not lose lighting. The shader
            // skips any fragment outside the grid, and a fragment outside the union
            // of every light's bound is outside every light's range by construction.
            Vector3 bMin(std::numeric_limits<float>::max());
            Vector3 bMax(std::numeric_limits<float>::lowest());

            for (const auto& light : _lights) {
                const auto lightMin = light.aabb.center() - light.aabb.halfExtents();
                const auto lightMax = light.aabb.center() + light.aabb.halfExtents();

                bMin = Vector3::min(bMin, lightMin);
                bMax = Vector3::max(bMax, lightMax);
            }

            _boundsMin = bMin;
            _boundsMax = bMax;
        }

        // Add small epsilon padding to prevent division by zero.
        constexpr float eps = 0.001f;
        const auto range = _boundsMax - _boundsMin;
        const Vector3 padding(range.getX() < eps ? eps : 0.0f, range.getY() < eps ? eps : 0.0f,
            range.getZ() < eps ? eps : 0.0f);
        _boundsMax = _boundsMax + padding;
    }

    void WorldClusters::assignLightsToCells()
    {
        const size_t cellCount = static_cast<size_t>(_config.totalCells());
        const int maxPerCell = _config.maxLightsPerCell;
        const size_t cellDataSize = cellCount * static_cast<size_t>(maxPerCell);

        // Resize and clear cell data.
        if (_cellData.size() != cellDataSize) {
            _cellData.resize(cellDataSize);
        }
        std::memset(_cellData.data(), 0, _cellData.size());

        if (_lights.empty()) {
            return;
        }

        const auto range = boundsRange();
        const float invRangeX = range.getX() > 1e-6f ? 1.0f / range.getX() : 0.0f;
        const float invRangeY = range.getY() > 1e-6f ? 1.0f / range.getY() : 0.0f;
        const float invRangeZ = range.getZ() > 1e-6f ? 1.0f / range.getZ() : 0.0f;
        const Vector3 invRange(invRangeX, invRangeY, invRangeZ);
        const Vector3 cells(static_cast<float>(_config.cellsX), static_cast<float>(_config.cellsY),
            static_cast<float>(_config.cellsZ));

        // Track light count per cell for fast insertion.
        // Use a temporary vector since we only need counts during assignment.
        std::vector<int> cellCounts(cellCount, 0);

        for (int lightIdx = 0; lightIdx < static_cast<int>(_lights.size()); ++lightIdx) {
            const auto& light = _lights[lightIdx];

            // Convert light AABB to cell coordinates.
            const auto lMin = light.aabb.center() - light.aabb.halfExtents();
            const auto lMax = light.aabb.center() + light.aabb.halfExtents();

            const Vector3 cellMin = ((lMin - _boundsMin) * invRange * cells).floor();
            const Vector3 cellMax = ((lMax - _boundsMin) * invRange * cells).floor();

            int cellMinX = static_cast<int>(cellMin.getX());
            int cellMinY = static_cast<int>(cellMin.getY());
            int cellMinZ = static_cast<int>(cellMin.getZ());
            int cellMaxX = static_cast<int>(cellMax.getX());
            int cellMaxY = static_cast<int>(cellMax.getY());
            int cellMaxZ = static_cast<int>(cellMax.getZ());

            // Clamp to valid cell range.
            cellMinX = std::clamp(cellMinX, 0, _config.cellsX - 1);
            cellMinY = std::clamp(cellMinY, 0, _config.cellsY - 1);
            cellMinZ = std::clamp(cellMinZ, 0, _config.cellsZ - 1);
            cellMaxX = std::clamp(cellMaxX, 0, _config.cellsX - 1);
            cellMaxY = std::clamp(cellMaxY, 0, _config.cellsY - 1);
            cellMaxZ = std::clamp(cellMaxZ, 0, _config.cellsZ - 1);

            // Store light index (1-based) in each overlapping cell.
            const uint8_t lightIdx1 = static_cast<uint8_t>(lightIdx + 1);

            for (int y = cellMinY; y <= cellMaxY; ++y) {
                for (int z = cellMinZ; z <= cellMaxZ; ++z) {
                    for (int x = cellMinX; x <= cellMaxX; ++x) {
                        const int cellIndex = y * _config.cellsX * _config.cellsZ
                                            + z * _config.cellsX
                                            + x;
                        const int count = cellCounts[cellIndex];
                        if (count < maxPerCell) {
                            _cellData[static_cast<size_t>(cellIndex * maxPerCell + count)] = lightIdx1;
                            cellCounts[cellIndex] = count + 1;
                        } else if (!_warnedOverflow) {
                            spdlog::warn("WorldClusters: cell ({},{},{}) exceeded maxLightsPerCell={}, some lights dropped",
                                x, y, z, maxPerCell);
                            _warnedOverflow = true;
                        }
                    }
                }
            }
        }
    }

    void WorldClusters::packGpuLights()
    {
        _gpuLights.resize(_lights.size());

        for (size_t i = 0; i < _lights.size(); ++i) {
            const auto& entry = _lights[i];
            const auto& ld = entry.data;
            auto& gpu = _gpuLights[i];

            ld.position.store(gpu.positionRange);
            gpu.positionRange[3] = ld.range;

            ld.direction.store(gpu.directionSpot);
            gpu.directionSpot[3] = entry.outerConeCos;

            // Convert sRGB color to linear for GPU.
            const float r = std::pow(std::max(ld.color.r, 0.0f), 2.2f);
            const float g = std::pow(std::max(ld.color.g, 0.0f), 2.2f);
            const float b = std::pow(std::max(ld.color.b, 0.0f), 2.2f);
            gpu.colorIntensity[0] = r;
            gpu.colorIntensity[1] = g;
            gpu.colorIntensity[2] = b;
            gpu.colorIntensity[3] = ld.intensity;

            gpu.params[0] = entry.innerConeCos;
            gpu.params[1] = ld.isSpot ? 1.0f : 0.0f;
            gpu.params[2] = ld.falloffModeLinear ? 1.0f : 0.0f;
            gpu.params[3] = 0.0f;

            // Clustered shadow data (via LightTextureAtlas). The renderer sets
            // castShadows only for a light the atlas gave a slot this frame.
            const bool hasShadow = ld.castShadows;
            gpu.shadowData[0] = hasShadow ? 1.0f : 0.0f;
            gpu.shadowData[1] = ld.shadowNormalBias;
            gpu.shadowData[2] = ld.shadowIntensity;
            gpu.shadowData[3] = ld.isSpot ? 1.0f : 2.0f;
            if (hasShadow && !ld.isSpot) {
                // Omni: no matrix — the rect and the depth range, in the same 64
                // bytes (see GpuClusteredLight). The shader derives the face and its
                // UV from the direction, as a cubemap lookup would.
                for (float& value : gpu.shadowMatrix) {
                    value = 0.0f;
                }
                ld.atlasViewport.store(gpu.shadowMatrix);  // xyz; w is overwritten below
                gpu.shadowMatrix[3] = static_cast<float>(LightTextureAtlas::kShadowEdgePixels);
                gpu.shadowMatrix[4] = ld.shadowNear;
                gpu.shadowMatrix[5] = ld.shadowFar;
                gpu.shadowMatrix[6] = ld.shadowRelativeBias;
                gpu.shadowMatrix[7] = 0.0f;
            } else if (hasShadow) {
                // Column-major float4x4 for the GPU (dest[col*4+row] = M(row,col)),
                // which is exactly Matrix4's own storage order. An element loop here
                // once uploaded the transpose, which put every receiver's shadow
                // coordinate outside [0,1] so the clustered shadow test silently fell
                // through to unshadowed on both backends.
                ld.shadowMatrix.store(gpu.shadowMatrix);
            }
        }
    }
}
