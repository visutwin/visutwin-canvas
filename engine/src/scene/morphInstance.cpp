// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "morphInstance.h"

#include <algorithm>
#include <cmath>

#include "morph.h"

namespace visutwin::canvas
{
    MorphInstance::MorphInstance(std::shared_ptr<Morph> morph)
        : _morph(std::move(morph))
    {
        if (_morph) {
            _weights.resize(_morph->targetCount(), 0.0f);
        }
    }

    void MorphInstance::setWeight(const int targetIndex, const float weight)
    {
        if (targetIndex < 0 || targetIndex >= static_cast<int>(_weights.size())) {
            return;
        }
        if (_weights[targetIndex] != weight) {
            _weights[targetIndex] = weight;
            _dirty = true;
        }
    }

    float MorphInstance::weight(const int targetIndex) const
    {
        return (targetIndex >= 0 && targetIndex < static_cast<int>(_weights.size()))
            ? _weights[targetIndex] : 0.0f;
    }

    const MorphInstance::GpuMorphParams& MorphInstance::gpuParams()
    {
        if (!_dirty) {
            return _gpuParams;
        }
        _dirty = false;

        _gpuParams = GpuMorphParams{};
        _gpuParams.vertexCount = _morph ? static_cast<uint32_t>(_morph->vertexCount()) : 0;

        // Collect active targets, strongest |weight| first, capped at MAX_ACTIVE_TARGETS
        // (matches upstream's active-target sorting in morph-instance.js).
        constexpr float epsilon = 1e-5f;
        std::vector<int> active;
        active.reserve(_weights.size());
        for (int i = 0; i < static_cast<int>(_weights.size()); ++i) {
            if (std::abs(_weights[i]) > epsilon) {
                active.push_back(i);
            }
        }
        if (static_cast<int>(active.size()) > MAX_ACTIVE_TARGETS) {
            std::partial_sort(active.begin(), active.begin() + MAX_ACTIVE_TARGETS, active.end(),
                [this](const int a, const int b) {
                    return std::abs(_weights[a]) > std::abs(_weights[b]);
                });
            active.resize(MAX_ACTIVE_TARGETS);
        }

        _gpuParams.activeCount = static_cast<uint32_t>(active.size());
        for (size_t k = 0; k < active.size(); ++k) {
            _gpuParams.indices[k] = static_cast<uint32_t>(active[k]);
            _gpuParams.weights[k] = _weights[active[k]];
        }
        return _gpuParams;
    }
}
