// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//

#include "worldClusters.h"

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

    void WorldClusters::update(const std::vector<ClusterLightData>& localLights,
                               const BoundingBox& cameraBounds)
    {
        _warnedOverflow = false;

        collectLights(localLights);
        computeGridBounds(cameraBounds);
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
            entry.outerConeCos = std::cos(toRadians(std::max(ld.outerConeAngle, 0.0f) * 0.5f));
            entry.innerConeCos = std::cos(toRadians(std::max(ld.innerConeAngle, 0.0f) * 0.5f));
            if (entry.innerConeCos < entry.outerConeCos) {
                entry.innerConeCos = entry.outerConeCos;
            }

            // Compute world-space AABB for grid assignment.
            if (ld.isSpot) {
                // Spot light: cone-shaped AABB.
                // Approximate: the cone fits in a sphere of radius=range centered at
                // a point offset from the light position along the light direction.
                // For simplicity, use the full sphere AABB (slightly conservative).
                const float r = ld.range;
                entry.aabb = BoundingBox(ld.position, Vector3(r, r, r));
            } else {
                // Point/omni light: sphere AABB.
                const float r = ld.range;
                entry.aabb = BoundingBox(ld.position, Vector3(r, r, r));
            }

            _lights.push_back(entry);
        }
    }

    void WorldClusters::computeGridBounds(const BoundingBox& cameraBounds)
    {
        if (_lights.empty()) {
            _boundsMin = cameraBounds.center() - cameraBounds.halfExtents();
            _boundsMax = cameraBounds.center() + cameraBounds.halfExtents();
        } else {
            // Start with camera bounds, expand to include all light AABBs.
            Vector3 bMin = cameraBounds.center() - cameraBounds.halfExtents();
            Vector3 bMax = cameraBounds.center() + cameraBounds.halfExtents();

            for (const auto& light : _lights) {
                const auto lightMin = light.aabb.center() - light.aabb.halfExtents();
                const auto lightMax = light.aabb.center() + light.aabb.halfExtents();

                bMin = Vector3(
                    std::min(bMin.getX(), lightMin.getX()),
                    std::min(bMin.getY(), lightMin.getY()),
                    std::min(bMin.getZ(), lightMin.getZ())
                );
                bMax = Vector3(
                    std::max(bMax.getX(), lightMax.getX()),
                    std::max(bMax.getY(), lightMax.getY()),
                    std::max(bMax.getZ(), lightMax.getZ())
                );
            }

            _boundsMin = bMin;
            _boundsMax = bMax;
        }

        // Add small epsilon padding to prevent division by zero.
        constexpr float eps = 0.001f;
        const auto range = _boundsMax - _boundsMin;
        if (range.getX() < eps) { _boundsMax = Vector3(_boundsMax.getX() + eps, _boundsMax.getY(), _boundsMax.getZ()); }
        if (range.getY() < eps) { _boundsMax = Vector3(_boundsMax.getX(), _boundsMax.getY() + eps, _boundsMax.getZ()); }
        if (range.getZ() < eps) { _boundsMax = Vector3(_boundsMax.getX(), _boundsMax.getY(), _boundsMax.getZ() + eps); }
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

        // Track light count per cell for fast insertion.
        // Use a temporary vector since we only need counts during assignment.
        std::vector<int> cellCounts(cellCount, 0);

        for (int lightIdx = 0; lightIdx < static_cast<int>(_lights.size()); ++lightIdx) {
            const auto& light = _lights[lightIdx];

            // Convert light AABB to cell coordinates.
            const auto lMin = light.aabb.center() - light.aabb.halfExtents();
            const auto lMax = light.aabb.center() + light.aabb.halfExtents();

            int cellMinX = static_cast<int>(std::floor((lMin.getX() - _boundsMin.getX()) * invRangeX * static_cast<float>(_config.cellsX)));
            int cellMinY = static_cast<int>(std::floor((lMin.getY() - _boundsMin.getY()) * invRangeY * static_cast<float>(_config.cellsY)));
            int cellMinZ = static_cast<int>(std::floor((lMin.getZ() - _boundsMin.getZ()) * invRangeZ * static_cast<float>(_config.cellsZ)));
            int cellMaxX = static_cast<int>(std::floor((lMax.getX() - _boundsMin.getX()) * invRangeX * static_cast<float>(_config.cellsX)));
            int cellMaxY = static_cast<int>(std::floor((lMax.getY() - _boundsMin.getY()) * invRangeY * static_cast<float>(_config.cellsY)));
            int cellMaxZ = static_cast<int>(std::floor((lMax.getZ() - _boundsMin.getZ()) * invRangeZ * static_cast<float>(_config.cellsZ)));

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

            gpu.positionRange[0] = ld.position.getX();
            gpu.positionRange[1] = ld.position.getY();
            gpu.positionRange[2] = ld.position.getZ();
            gpu.positionRange[3] = ld.range;

            gpu.directionSpot[0] = ld.direction.getX();
            gpu.directionSpot[1] = ld.direction.getY();
            gpu.directionSpot[2] = ld.direction.getZ();
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
        }
    }
}
