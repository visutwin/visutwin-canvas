// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 06.11.2026
//
// The material block's map lanes (MaterialUniforms::mapLanes, bindlessResidency.h): one per
// map the forward and shadow programs sample, in this order, each named by the MATERIAL
// SLOT the map is set under (TextureSlot::slot, the number a material has always named its
// textures by) and whether a map on that slot keeps its texture's own sampler state (the
// rows that bound a combined sampler before the table) or reads through sampler 0, the
// default state. tools/generate_shader_bindings.py reads this list into bindings.slang as
// VT_MAP_LANE_<name>, so the shaders index the lanes by the same names.
#pragma once

#include <cstdint>

namespace visutwin::canvas
{
#define VT_MATERIAL_MAP_LANES(X) \
    X(baseColorMap,               0,   1) \
    X(normalMap,                  1,   1) \
    X(metalRoughMap,              3,   1) \
    X(occlusionMap,               4,   1) \
    X(emissiveMap,                5,   1) \
    X(clearCoatMap,               7,   0) \
    X(clearCoatGlossMap,          13,  0) \
    X(clearCoatNormalMap,         14,  0) \
    X(heightMap,                  17,  0) \
    X(lightMap,                   19,  1) \
    X(detailNormalMap,            23,  0) \
    X(glossMap,                   31,  0) \
    X(thicknessMap,               32,  0) \
    X(refractionMap,              33,  0) \
    X(opacityMap,                 34,  0) \
    X(anisotropyMap,              42,  0) \
    X(specularColorMap,           43,  0) \
    X(specularityFactorMap,       44,  0) \
    X(materialEnvAtlas,           45,  0) \
    X(iridescenceMap,             47,  0) \
    X(iridescenceThicknessMap,    48,  0) \
    X(diffuseTransmissionMap,     49,  0) \
    X(diffuseTransmissionColorMap, 50, 0) \
    X(planarReflectionMap,        51,  0) \
    X(displacementMap,            141, 0)

    enum class MaterialMapLane : uint32_t
    {
#define VT_MATERIAL_MAP_LANE_ENUM(name, slot, ownSampler) name,
        VT_MATERIAL_MAP_LANES(VT_MATERIAL_MAP_LANE_ENUM)
#undef VT_MATERIAL_MAP_LANE_ENUM
        Count
    };
    inline constexpr uint32_t kMaterialMapLaneCount = static_cast<uint32_t>(MaterialMapLane::Count);
    static_assert(kMaterialMapLaneCount == 25, "mapLanes holds 28 lanes (7 uint4); 25 are named");

    struct MaterialMapLaneRow
    {
        int slot;
        bool ownSampler;
    };
#define VT_MATERIAL_MAP_LANE_ROW(name, slot, ownSampler) MaterialMapLaneRow{slot, ownSampler != 0},
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
    /// Whether a map in `lane` keeps its texture's own sampler state.
    constexpr bool materialMapLaneOwnSampler(const uint32_t lane)
    {
        return lane < kMaterialMapLaneCount && kMaterialMapLaneRows[lane].ownSampler;
    }
    static_assert(materialMapLaneForSlot(0) == 0 && materialMapLaneForSlot(19) == 9 &&
        materialMapLaneForSlot(141) == 24 && materialMapLaneForSlot(2) == -1 && materialMapLaneForSlot(9) == -1);
}
