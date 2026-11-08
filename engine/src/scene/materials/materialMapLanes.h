// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 06.11.2026
//
// The material block's map lanes (MaterialUniforms::mapLanes, bindlessResidency.h): one per
// map the forward and shadow programs sample, in this order, each named by the MATERIAL
// SLOT the map is set under (TextureSlot::slot, the number a material has always named its
// textures by). Every map reads through its texture's own sampler state (sampler 0 when the
// texture is in the default state). tools/generate_shader_bindings.py reads this list into bindings.slang as
// VT_MAP_LANE_<name>, so the shaders index the lanes by the same names.
#pragma once

#include <cstdint>

namespace visutwin::canvas
{
#define VT_MATERIAL_MAP_LANES(X) \
    X(baseColorMap,               0) \
    X(normalMap,                  1) \
    X(metalRoughMap,              3) \
    X(occlusionMap,               4) \
    X(emissiveMap,                5) \
    X(clearCoatMap,               7) \
    X(clearCoatGlossMap,          13) \
    X(clearCoatNormalMap,         14) \
    X(heightMap,                  17) \
    X(lightMap,                   19) \
    X(detailNormalMap,            23) \
    X(glossMap,                   31) \
    X(thicknessMap,               32) \
    X(refractionMap,              33) \
    X(opacityMap,                 34) \
    X(anisotropyMap,              42) \
    X(specularColorMap,           43) \
    X(specularityFactorMap,       44) \
    X(materialEnvAtlas,           45) \
    X(iridescenceMap,             47) \
    X(iridescenceThicknessMap,    48) \
    X(diffuseTransmissionMap,     49) \
    X(diffuseTransmissionColorMap, 50) \
    X(planarReflectionMap,        51) \
    X(displacementMap,            141)

    /// The material slot of the vertex-stage displacement map (StandardMaterial::setDisplacementMap),
    /// a lane and no row: it was vertex texture 41 before the table.
    inline constexpr int kDisplacementMapMetalSlot = 141;

    /// The per-draw lanes block (bindlessTable.h): lane i at [i / 4][i % 4], the texture index in
    /// the low 24 bits and the sampler index above, 0 for no map. Packed by
    /// Material::packedMapLanes and uploaded per draw by the device with the instance's own
    /// lightmap written over the lightmap lane (GraphicsDevice::drawMapLanes).
    struct MaterialMapLanes
    {
        uint32_t lanes[7][4] = {};

        uint32_t& at(const uint32_t lane) { return lanes[lane >> 2][lane & 3]; }
        uint32_t at(const uint32_t lane) const { return lanes[lane >> 2][lane & 3]; }
        bool operator==(const MaterialMapLanes&) const = default;
    };
    static_assert(sizeof(MaterialMapLanes) == 112);

    enum class MaterialMapLane : uint32_t
    {
#define VT_MATERIAL_MAP_LANE_ENUM(name, slot) name,
        VT_MATERIAL_MAP_LANES(VT_MATERIAL_MAP_LANE_ENUM)
#undef VT_MATERIAL_MAP_LANE_ENUM
        Count
    };
    inline constexpr uint32_t kMaterialMapLaneCount = static_cast<uint32_t>(MaterialMapLane::Count);
    static_assert(kMaterialMapLaneCount == 25, "mapLanes holds 28 lanes (7 uint4); 25 are named");

    struct MaterialMapLaneRow
    {
        int slot;
    };
#define VT_MATERIAL_MAP_LANE_ROW(name, slot) MaterialMapLaneRow{slot},
    inline constexpr MaterialMapLaneRow kMaterialMapLaneRows[] = {VT_MATERIAL_MAP_LANES(VT_MATERIAL_MAP_LANE_ROW)};
#undef VT_MATERIAL_MAP_LANE_ROW

    /// The lane of a material slot, or -1 for a slot no lane names.
    constexpr int materialMapLaneForSlot(const int slot)
    {
        for (uint32_t lane = 0; lane < kMaterialMapLaneCount; ++lane) {
            if (kMaterialMapLaneRows[lane].slot == slot) {
                return static_cast<int>(lane);
            }
        }
        return -1;
    }
    static_assert(materialMapLaneForSlot(0) == 0 && materialMapLaneForSlot(19) == 9 &&
        materialMapLaneForSlot(141) == 24 && materialMapLaneForSlot(2) == -1 && materialMapLaneForSlot(9) == -1);
}
