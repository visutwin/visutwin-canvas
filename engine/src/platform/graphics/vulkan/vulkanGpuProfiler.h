// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#pragma once

#ifdef VISUTWIN_HAS_VULKAN

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "platform/graphics/gpuProfiler.h"

namespace visutwin::canvas::gpu
{
    /**
     * Vulkan GPU pass profiler: writes a timestamp query on either side of every
     * render pass and resolves the results two frames later, so reading them
     * never stalls the GPU.
     *
     * DEVIATION from the Metal backend: Metal samples at STAGE BOUNDARIES
     * (start-of-vertex → end-of-fragment) via MTLCounterSampleBuffer attached to
     * the pass descriptor, and converts ticks with correlated
     * MTLDevice::sampleTimestamps pairs. Vulkan has no stage-boundary sampling
     * for a render pass, so this brackets the pass with vkCmdWriteTimestamp2
     * (TOP_OF_PIPE before, ALL_COMMANDS after) recorded OUTSIDE the
     * vkCmdBeginRendering/EndRendering pair, and converts with the device's
     * fixed `timestampPeriod`. The bracket therefore includes any load/store
     * clear work the pass does, which the Metal numbers exclude — treat small
     * per-pass differences between backends as expected.
     *
     * Returns nullptr from create() when the queue family reports no valid
     * timestamp bits.
     */
    class VulkanGpuProfiler final : public GpuProfiler
    {
    public:
        /** Returns nullptr when the device/queue lacks timestamp support. */
        static std::shared_ptr<VulkanGpuProfiler> create(VkDevice device,
            VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex);

        ~VulkanGpuProfiler() override;

        /**
         * Rotate to the next frame slot and resolve the slot from 2 frames ago.
         * Must be called with a recording command buffer: resetting a query pool
         * is a device command, and the reset has to be ordered ahead of this
         * frame's writes.
         */
        void beginFrame(VkCommandBuffer cmd);

        /** Record the opening timestamp for one pass. Call before vkCmdBeginRendering. */
        void beginPass(VkCommandBuffer cmd, const std::string& name);

        /** Record the closing timestamp. Call after vkCmdEndRendering. */
        void endPass(VkCommandBuffer cmd);

    private:
        VulkanGpuProfiler(VkDevice device, float timestampPeriod);
        bool init();
        void resolveSlot(int slot);

        static constexpr int NUM_SLOTS = 3;          // frames in flight + resolve lag
        static constexpr int MAX_PASSES = 64;        // per frame
        static constexpr int SAMPLES_PER_PASS = 2;   // start + end

        struct FrameSlot
        {
            VkQueryPool queryPool = VK_NULL_HANDLE;
            std::vector<std::string> passNames;
            int passCount = 0;
            bool submitted = false;
        };

        VkDevice _device = VK_NULL_HANDLE;
        float _timestampPeriod = 1.0f;               // nanoseconds per tick
        std::array<FrameSlot, NUM_SLOTS> _slots;
        int _currentSlot = 0;
        uint64_t _frameIndex = 0;
        // -1 when no pass is open; otherwise the pass index whose closing
        // timestamp endPass() should write.
        int _openPass = -1;
    };
}

#endif // VISUTWIN_HAS_VULKAN
