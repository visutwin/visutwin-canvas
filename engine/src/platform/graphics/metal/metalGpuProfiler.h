// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <Metal/Metal.hpp>

#include "platform/graphics/gpuProfiler.h"

namespace visutwin::canvas::gpu
{
    /**
     * Metal GPU pass profiler: attaches an MTLCounterSampleBuffer to every render
     * pass descriptor (stage-boundary timestamps: start-of-vertex → end-of-fragment)
     * and resolves the samples two frames later. Tick→nanosecond conversion uses
     * correlated MTLDevice::sampleTimestamps pairs.
     *
     * Requires MTLCounterSamplingPointAtStageBoundary support (all Apple GPUs).
     */
    class MetalGpuProfiler final : public GpuProfiler
    {
    public:
        /** Returns nullptr when the device lacks timestamp counter support. */
        static std::shared_ptr<MetalGpuProfiler> create(MTL::Device* device);

        ~MetalGpuProfiler() override;

        /** Rotate to the next frame slot and resolve the slot from 2 frames ago. */
        void beginFrame();

        /** Attach timestamp sampling for one render pass to its descriptor. */
        void attachToRenderPass(MTL::RenderPassDescriptor* passDescriptor, const std::string& name);

    private:
        explicit MetalGpuProfiler(MTL::Device* device);
        bool init();
        void resolveSlot(int slot);

        static constexpr int NUM_SLOTS = 3;          // frames in flight + resolve lag
        static constexpr int MAX_PASSES = 64;        // per frame
        static constexpr int SAMPLES_PER_PASS = 2;   // start + end

        struct FrameSlot
        {
            MTL::CounterSampleBuffer* sampleBuffer = nullptr;
            std::vector<std::string> passNames;
            int passCount = 0;
        };

        MTL::Device* _device;
        std::array<FrameSlot, NUM_SLOTS> _slots;
        int _currentSlot = 0;
        uint64_t _frameIndex = 0;

        // Correlated CPU/GPU timestamp pair for tick→nanosecond conversion.
        MTL::Timestamp _baseCpuTimestamp = 0;
        MTL::Timestamp _baseGpuTimestamp = 0;
    };
}
