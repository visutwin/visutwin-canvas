// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 06.11.2026
//
// EXPERIMENTAL: the bindless material binding spike (ARCHITECTURE.md, "Design directions
// from the Bevy comparison", item 2). A table of textures and samplers that a Slang program
// indexes at run time, bound once for every draw that follows, instead of one slot of the
// binding table per map. Nothing in the engine's own programs reads one yet;
// tests/bindlessGpuTests.cpp proves the mechanism on both backends and reports the limits.
//
// A program declares the table as ONE parameter block, samplers FIRST:
//
//     struct Table { SamplerState samplers[S]; Texture2D textures[T]; };
//     [[vk::binding(0, kBindlessTableVulkanSet)]]
//     ParameterBlock<Table> table : register(b<kBindlessTableMetalBufferSlot>);
//
// with S and T the device's bindlessSamplerCapacity() / bindlessTextureCapacity(). On Metal
// that is one argument buffer at the buffer slot, on both stages (Slang declares every global
// on every entry), holding the samplers' and textures' GPU resource ids; on Vulkan a
// descriptor set with the samplers at binding 0 and the images at binding 1, both
// update-after-bind and partially bound. The arrays are SIZED on purpose: an unbounded
// array becomes a flexible array member in the MSL, which Apple's compiler rejects.
// Indexing is `VT_NONUNIFORM(i)`: NonUniformResourceIndex on SPIR-V (the decoration only
// survives when it wraps the index at the indexing expression itself, not inside a helper)
// and the bare index on Metal, where Slang refuses the intrinsic.
#pragma once

#include <cstdint>
#include <vector>

#include "textureSamplerState.h"

namespace visutwin::canvas
{
    class Texture;

    /// The Metal buffer slot the table's argument buffer binds at, vertex and fragment: 13
    /// is past every slot a draw uses (the splat SH buffer is the last, at vertex 12).
    inline constexpr int kBindlessTableMetalBufferSlot = 13;
    /// The Vulkan descriptor set: the eighth, after the seven of the draw layout (MoltenVK
    /// allows exactly eight).
    inline constexpr uint32_t kBindlessTableVulkanSet = 7;

    /// What a table holds: entry i of each array. A null texture leaves its entry unbound,
    /// which a program must not read. Both arrays must fit the device's capacities.
    struct BindlessTableDesc
    {
        std::vector<Texture*> textures;
        std::vector<TextureSamplerState> samplers;
    };

    /// A table built by GraphicsDevice::createBindlessTable: the backend's resources,
    /// released with it. It names the textures' GPU objects as they were at creation, so a
    /// texture resized or recreated afterwards (a render target made over it) is not seen.
    class BindlessTable
    {
    public:
        virtual ~BindlessTable() = default;
    };
}
