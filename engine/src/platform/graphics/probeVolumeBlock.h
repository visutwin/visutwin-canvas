// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
// The probe volume's part of the lighting block (LightingBlock::probeVolume), one layout on
// both backends: the forward program declares it field for field as `ProbeVolumeData`
// (engine/shaders/slang/forward/forward-fragment-head.slang) and reads it in
// forward/forward-fragment-ambient-modifier.slang. ProbeVolume (extras/probeVolume) fills it
// and hands it to the device (GraphicsDevice::setProbeVolumeBlocks), the renderer gives each
// layer the block for its camera, and each backend copies it into its lighting block.
//
#pragma once

#include <cstdint>

namespace visutwin::canvas
{
    struct ProbeVolumeBlock
    {
        float boxMin[4] = {};           // xyz the box of the volume
        float boxMax[4] = {};
        float invSize[4] = {};          // xyz 1 / the size of the box, 0 along an axis of no extent
        float count[4] = {1.0f, 1.0f, 1.0f, 0.0f};   // xyz the probes along each axis
        float cellSize[4] = {};         // xyz the size of a cell
        // normal bias, intensity, SH sharpening weight, leak reduction weight
        float params[4] = {};
        // 1 / the blend distance, the weight of the probes (0: the environment's ambient), the
        // reflection occlusion weight
        float blend[4] = {};
        float encoding[4] = {};         // 1 for 8-bit texels, the scale of L0 of 8-bit texels
        // 1 to use the visibility, the tiles along a row of its atlas, the texels along a side of a map
        float visibility[4] = {};
        float visibilityInvSize[4] = {1.0f, 1.0f, 0.0f, 0.0f};   // 1 / the size of the atlas
        // The bindless lanes (packBindlessLane) of the probe volume texture, the validity volume
        // and the visibility atlas; 0 reads the white placeholder.
        uint32_t lanes[4] = {};
        // While a bake lights its captured geometry (ProbeVolumeBaker, VT_FEATURE_PROBE_LIGHTING),
        // in the block of the cameras capturing for it: the lanes of the geometry atlas's albedo
        // and hit flag, its back face flag, distance and octahedral normal, its emission, and the
        // texture of the probes' capture positions; the lane of the sky atlas; and the atlases'
        // face size, probes along a row, the sky atlas's face size and the positions' width.
        uint32_t bakeLanes[4] = {};
        uint32_t bakeSkyLane[4] = {};
        float bakeAtlas[4] = {};
    };
    static_assert(sizeof(ProbeVolumeBlock) == 224, "the forward program declares this size");
}
