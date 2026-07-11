// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace visutwin::canvas
{
    class Morph;

    /**
     * An instance of a Morph with its own set of per-target weights.
     *
     * Each frame the renderer uploads the packed GpuMorphParams (the strongest
     * VT_MAX_ACTIVE_MORPHS active targets) alongside the shared Morph delta buffer;
     * the vertex shader sums the weighted deltas (see morph.h for the deviation note).
     */
    class MorphInstance
    {
    public:
        /** Maximum simultaneously-active targets the shader blends per draw. */
        static constexpr int MAX_ACTIVE_TARGETS = 8;

        /** Mirrors the MSL MorphParams struct at vertex buffer slot 10 (80 bytes). */
        struct GpuMorphParams
        {
            uint32_t activeCount = 0;
            uint32_t vertexCount = 0;
            uint32_t pad[2] = {0, 0};
            uint32_t indices[MAX_ACTIVE_TARGETS] = {};
            float weights[MAX_ACTIVE_TARGETS] = {};
        };
        static_assert(sizeof(GpuMorphParams) == 80);

        explicit MorphInstance(std::shared_ptr<Morph> morph);

        const std::shared_ptr<Morph>& morph() const { return _morph; }

        void setWeight(int targetIndex, float weight);
        float weight(int targetIndex) const;
        int weightCount() const { return static_cast<int>(_weights.size()); }

        /** Packed params for the current weights (recomputed lazily). */
        const GpuMorphParams& gpuParams();

    private:
        std::shared_ptr<Morph> _morph;
        std::vector<float> _weights;
        GpuMorphParams _gpuParams;
        bool _dirty = true;
    };
}
