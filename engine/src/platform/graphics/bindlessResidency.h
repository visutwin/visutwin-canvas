// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 06.11.2026
//
// The process-wide side of the bindless table (bindlessTable.h, ARCHITECTURE.md "Phase 1
// design"): which texture sits at which index, and which sampler state at which sampler
// index. A Texture takes an index the first time a material packs it (Texture::bindlessIndex)
// and gives it back in its destructor; its entry's VERSION moves on assignment, release and
// every recreation of its GPU object (Texture::noteGpuObjectChanged), which is what each
// device's per-frame table sync follows. The index space is global on purpose: a material's
// packed block carries indices and stays device-independent, and a device's table simply
// holds a white placeholder at an index whose texture is another device's.
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "textureSamplerState.h"

namespace visutwin::canvas
{
    class Texture;

    class BindlessResidency
    {
    public:
        /// Index 0 is "no map": no texture ever gets it.
        static constexpr uint32_t kNone = 0;

        struct Entry
        {
            Texture* texture = nullptr;
            uint64_t version = 0;   // 0 until the index is first used
        };

        /// Gives `texture` an index (the lowest free one; never 0).
        static uint32_t acquire(Texture* texture);
        /// Frees the index; its entry keeps a moved version and no texture.
        static void release(uint32_t index);
        /// The texture at `index` has a new GPU object: moves the entry's version.
        static void touch(uint32_t index);

        /// A copy of the entries (index -> entry; entry 0 unused), taken under the lock.
        static void snapshot(std::vector<Entry>& out);
        /// Moves with every acquire, release and touch: a sync that saw the same value can
        /// skip the snapshot.
        static uint64_t generation();
    };

    /// The sampler STATES a material map may be read with, by index; index 0 is the default
    /// state (repeat, trilinear, anisotropic). Keyed without a device: a state is registered
    /// with kAnisotropy, and each device creates its sampler clamped to its own maximum.
    class BindlessSamplers
    {
    public:
        static constexpr float kAnisotropy = 16.0f;

        static uint32_t indexOf(const TextureSamplerState& state);
        static TextureSamplerState defaultState();
        /// A copy of the states, index order.
        static void snapshot(std::vector<TextureSamplerState>& out);
        static uint64_t generation();
    };

    /// A material block lane: the texture index in the low 24 bits, the sampler index above.
    inline constexpr uint32_t packBindlessLane(const uint32_t textureIndex, const uint32_t samplerIndex)
    {
        return (textureIndex & 0xFFFFFFu) | (samplerIndex << 24u);
    }
    inline constexpr uint32_t bindlessLaneTexture(const uint32_t lane) { return lane & 0xFFFFFFu; }
    inline constexpr uint32_t bindlessLaneSampler(const uint32_t lane) { return lane >> 24u; }
}
