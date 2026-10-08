// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// Metal-optimized: uses Metal buffers rather than a texture-based cell layout.
//
#pragma once

#include <cstdint>
#include <vector>

#include "core/math/color.h"
#include "core/math/matrix4.h"
#include "core/math/vector3.h"
#include "core/math/vector4.h"
#include "core/shape/boundingBox.h"
#include "scene/constants.h"

namespace visutwin::canvas
{
    /**
     * Configuration for the 3D cluster grid.
     */
    struct ClusterConfig
    {
        int cellsX = 12;
        int cellsY = 16;
        int cellsZ = 12;
        // Max lights bucketed per grid cell.
        // The fragment shader loops up to this count dynamically, so raising it only
        // costs a little cell-index memory (totalCells * maxLightsPerCell bytes).
        int maxLightsPerCell = 48;

        int totalCells() const { return cellsX * cellsY * cellsZ; }
    };

    /**
     * GPU-side packed light struct (176 bytes, 16-byte aligned).
     * Maps 1:1 to the ClusterLight struct in engine/shaders/slang/forward/forward-fragment-head.slang.
     */
    struct alignas(16) GpuClusteredLight
    {
        float positionRange[4] = {};     // xyz=position, w=range
        float directionSpot[4] = {};     // xyz=direction, w=outerConeCos
        float colorIntensity[4] = {};    // xyz=color(linear), w=intensity, from lightRadiance
        // x=innerConeCos, y=isSpot(0/1), z=falloffLinear(0/1), w=cookie: 0 for none,
        // else (CookieChannel + 1) * 2 + intensity (intensity in [0, 1]), the shader's
        // decodeClusterCookie. A cookie reads the same shadowMatrix as the shadow does
        // (a spot's projection into its rect, an omni's rect), shadowed or not.
        float params[4] = {};
        // Clustered shadows (LightTextureAtlas). A SPOT: world→atlas-rect shadow VP,
        // column-major float4x4. An OMNI light has no single matrix: the same 64
        // bytes carry its atlas rect (x, y, size, edge pixels) in the first column
        // and (near, far, relative bias, unused) in the second; the shader picks
        // the cube face from the light-to-fragment direction.
        float shadowMatrix[16] = {};
        float shadowData[4] = {};        // x=castShadows(0/1), y=normalOffsetBias,
                                         // z=intensity, w=1 spot / 2 omni
        // An area light: the world half axes, and the
        // LightShape in areaHalfWidth.w (0 = punctual, which shades as before).
        // areaHalfHeight.w carries the light's MASK_AFFECT_DYNAMIC (1) and
        // MASK_AFFECT_LIGHTMAPPED (2) bits as a float: the shader skips the light for a
        // draw whose accepted bit (LightingUniforms clusterParams2.y, from the draw's
        // mesh-instance mask) it lacks.
        float areaHalfWidth[4] = {};
        float areaHalfHeight[4] = {};
    };
    static_assert(sizeof(GpuClusteredLight) == 176);

    /**
     * Lightweight data struct for passing light properties to WorldClusters.
     * Avoids coupling WorldClusters to LightComponent internals.
     */
    struct ClusterLightData
    {
        Vector3 position;
        Vector3 direction = Vector3(0.0f, -1.0f, 0.0f);
        Color color = Color(1.0f, 1.0f, 1.0f, 1.0f);
        float intensity = 0.0f;
        float range = 10.0f;
        float innerConeAngle = 0.0f;
        float outerConeAngle = 45.0f;
        bool isSpot = false;
        bool falloffModeLinear = true;
        // The light's mask. Only its MASK_AFFECT_DYNAMIC and MASK_AFFECT_LIGHTMAPPED bits
        // reach the GPU; a light with neither (MASK_BAKE alone) is not clustered at all.
        uint32_t lightMask = MASK_AFFECT_DYNAMIC;

        // Clustered shadow (via LightTextureAtlas). castShadows=false → no shadow.
        bool castShadows = false;
        Matrix4 shadowMatrix = Matrix4::identity();  // spot: world→atlas-rect shadow VP
        Vector4 atlasViewport = Vector4(0.0f, 0.0f, 1.0f, 1.0f);  // the light's slot
        // Omni faces store perspective depth over [shadowNear, shadowFar] and take
        // a RELATIVE bias on the distance, as the cubemap path does.
        float shadowNear = 0.01f;
        float shadowFar = 10.0f;
        float shadowRelativeBias = 0.0f;
        // A clustered spot shadow applies NO depth bias in the shader (the pass
        // sets hardware polygon offset). What it DOES apply is a normal offset on the receiver.
        float shadowNormalBias = 0.0f;
        float shadowIntensity = 1.0f;

        // Clustered cookie: the light's cookie, copied into the
        // same rect of the cookie atlas. A spot without a shadow still fills
        // `shadowMatrix` with its projection into that rect, and an omni `atlasViewport`.
        bool hasCookie = false;
        float cookieIntensity = 1.0f;
        uint32_t cookieChannel = 0;   // CookieChannel: 0 rgb, 1 r, 2 g, 3 b, 4 a

        // An area light's shape (LightShape) and world half axes; 0 = punctual.
        uint32_t shape = 0;
        Vector3 areaHalfWidth = Vector3(0.0f);
        Vector3 areaHalfHeight = Vector3(0.0f);
    };

    /// Whether a light with this mask is lit at runtime, and so belongs in a cluster grid:
    /// it must affect dynamic or lightmapped meshes. A bake-only light (MASK_BAKE) lights
    /// nothing at runtime.
    inline bool clusterAdmitsLight(const uint32_t lightMask)
    {
        return (lightMask & (MASK_AFFECT_DYNAMIC | MASK_AFFECT_LIGHTMAPPED)) != 0u;
    }

    /**
     * CPU-side 3D grid clustering for local lights (point/spot).
     *
     * Divides the world into a 3D grid. Each cell stores indices of lights
     * whose AABB overlaps that cell. The GPU fragment shader looks up its
     * world position in the grid and only evaluates lights in the matching cell.
     *
     * WorldClusters adapted for Metal buffer access.
     * CPU-only: produces data arrays that the renderer uploads to GPU buffers.
     */
    class WorldClusters
    {
    public:
        explicit WorldClusters(const ClusterConfig& config = ClusterConfig{});

        /**
         * Called per frame: collect lights, build grid, pack GPU data arrays.
         * @param localLights  Local lights (point/spot) collected from LightComponent.
         *
         * The grid is sized from the LIGHTS alone. It used
         * to take a camera AABB too, which padded the grid to a 100-unit cube around
         * the viewer and coarsened every cell; the camera has nothing to say about
         * where the lights are.
         */
        void update(const std::vector<ClusterLightData>& localLights);

        // CPU data arrays for GPU upload by the renderer/device.
        const GpuClusteredLight* lightData() const { return _gpuLights.data(); }
        size_t lightDataSize() const { return _gpuLights.size() * sizeof(GpuClusteredLight); }
        int lightCount() const { return static_cast<int>(_gpuLights.size()); }

        const uint8_t* cellData() const { return _cellData.data(); }
        size_t cellDataSize() const { return _cellData.size(); }

        // Grid params for shader uniforms.
        const Vector3& boundsMin() const { return _boundsMin; }
        Vector3 boundsRange() const { return _boundsMax - _boundsMin; }
        Vector3 cellsCountByBoundsSize() const;

        const ClusterConfig& config() const { return _config; }

    private:
        void collectLights(const std::vector<ClusterLightData>& localLights);
        void computeGridBounds();
        void assignLightsToCells();
        void packGpuLights();

        ClusterConfig _config;

        // CPU-side light entries with computed AABBs.
        struct LightEntry
        {
            ClusterLightData data;
            float outerConeCos = 1.0f;
            float innerConeCos = 1.0f;
            BoundingBox aabb;
        };

        std::vector<LightEntry> _lights;
        std::vector<GpuClusteredLight> _gpuLights;

        // Flat cell data: totalCells * maxLightsPerCell uint8 indices (1-based; 0=empty).
        std::vector<uint8_t> _cellData;

        Vector3 _boundsMin = Vector3(0.0f);
        Vector3 _boundsMax = Vector3(0.0f);

        bool _warnedOverflow = false;
    };
}
