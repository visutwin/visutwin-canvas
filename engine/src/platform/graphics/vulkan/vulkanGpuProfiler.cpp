// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanGpuProfiler.h"

#include <vector>

#include "spdlog/spdlog.h"

namespace visutwin::canvas::gpu
{
    std::shared_ptr<VulkanGpuProfiler> VulkanGpuProfiler::create(
        const VkDevice device, const VkPhysicalDevice physicalDevice,
        const uint32_t queueFamilyIndex)
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physicalDevice, &props);
        if (props.limits.timestampPeriod <= 0.0f) {
            spdlog::info("Vulkan GPU profiler unavailable: device reports no "
                "timestamp period");
            return nullptr;
        }

        // timestampPeriod being non-zero is not enough — the QUEUE FAMILY has to
        // support timestamps, reported as a bit count rather than a bool.
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount,
            families.data());
        if (queueFamilyIndex >= familyCount ||
            families[queueFamilyIndex].timestampValidBits == 0) {
            spdlog::info("Vulkan GPU profiler unavailable: queue family {} has no "
                "valid timestamp bits", queueFamilyIndex);
            return nullptr;
        }

        auto profiler = std::shared_ptr<VulkanGpuProfiler>(
            new VulkanGpuProfiler(device, props.limits.timestampPeriod));
        if (!profiler->init()) {
            return nullptr;
        }
        return profiler;
    }

    VulkanGpuProfiler::VulkanGpuProfiler(const VkDevice device,
        const float timestampPeriod)
        : _device(device), _timestampPeriod(timestampPeriod)
    {
    }

    VulkanGpuProfiler::~VulkanGpuProfiler()
    {
        for (auto& slot : _slots) {
            if (slot.queryPool != VK_NULL_HANDLE) {
                vkDestroyQueryPool(_device, slot.queryPool, nullptr);
                slot.queryPool = VK_NULL_HANDLE;
            }
        }
    }

    bool VulkanGpuProfiler::init()
    {
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = MAX_PASSES * SAMPLES_PER_PASS;

        for (auto& slot : _slots) {
            if (vkCreateQueryPool(_device, &info, nullptr, &slot.queryPool) !=
                VK_SUCCESS) {
                spdlog::warn("Vulkan GPU profiler: query pool creation failed; "
                    "profiling disabled");
                return false;
            }
            slot.passNames.reserve(MAX_PASSES);
        }
        return true;
    }

    void VulkanGpuProfiler::beginFrame(const VkCommandBuffer cmd)
    {
        if (!_enabled || cmd == VK_NULL_HANDLE) {
            return;
        }

        _currentSlot = static_cast<int>(_frameIndex % NUM_SLOTS);
        ++_frameIndex;

        // Resolve the slot recorded NUM_SLOTS-1 frames ago. By now its submission
        // has completed, so reading it never blocks — the same 2-frame lag the
        // Metal profiler has.
        if (_frameIndex > static_cast<uint64_t>(NUM_SLOTS)) {
            resolveSlot(static_cast<int>(_frameIndex % NUM_SLOTS));
        }

        auto& slot = _slots[_currentSlot];
        // The whole pool is reset even though only passCount*2 queries were used
        // last time: unwritten queries stay in an undefined state otherwise, and
        // vkGetQueryPoolResults on them would report garbage availability.
        vkCmdResetQueryPool(cmd, slot.queryPool, 0, MAX_PASSES * SAMPLES_PER_PASS);
        slot.passCount = 0;
        slot.passNames.clear();
        slot.submitted = true;
        _openPass = -1;
    }

    void VulkanGpuProfiler::beginPass(const VkCommandBuffer cmd,
        const std::string& name)
    {
        if (!_enabled || cmd == VK_NULL_HANDLE) {
            return;
        }
        auto& slot = _slots[_currentSlot];
        if (!slot.submitted || slot.passCount >= MAX_PASSES) {
            // Either beginFrame() was never called for this frame (profiling
            // switched on mid-frame) or the frame has more passes than the pool
            // holds. Dropping the pass is better than writing a query index the
            // reset never covered.
            _openPass = -1;
            return;
        }

        const int passIndex = slot.passCount;
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            slot.queryPool, static_cast<uint32_t>(passIndex * SAMPLES_PER_PASS));
        slot.passNames.push_back(name);
        slot.passCount = passIndex + 1;
        _openPass = passIndex;
    }

    void VulkanGpuProfiler::endPass(const VkCommandBuffer cmd)
    {
        if (!_enabled || cmd == VK_NULL_HANDLE || _openPass < 0) {
            return;
        }
        auto& slot = _slots[_currentSlot];
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            slot.queryPool,
            static_cast<uint32_t>(_openPass * SAMPLES_PER_PASS + 1));
        _openPass = -1;
    }

    void VulkanGpuProfiler::resolveSlot(const int slotIndex)
    {
        auto& slot = _slots[slotIndex];
        if (slot.passCount == 0) {
            return;
        }

        const auto queryCount =
            static_cast<uint32_t>(slot.passCount * SAMPLES_PER_PASS);
        // Two uint64s per query: the tick, then its availability. Asking for
        // availability instead of WAIT keeps this non-blocking — a pass whose
        // result is not ready yet is reported as 0 ms rather than stalling.
        std::vector<uint64_t> results(queryCount * 2, 0);
        const VkResult status = vkGetQueryPoolResults(_device, slot.queryPool, 0,
            queryCount, results.size() * sizeof(uint64_t), results.data(),
            2 * sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (status != VK_SUCCESS && status != VK_NOT_READY) {
            slot.passCount = 0;
            return;
        }

        _passTimings.clear();
        _frameMilliseconds = 0.0;
        for (int i = 0; i < slot.passCount; ++i) {
            const size_t startIndex = static_cast<size_t>(i) * SAMPLES_PER_PASS * 2;
            const size_t endIndex = startIndex + 2;
            const uint64_t start = results[startIndex];
            const bool startAvailable = results[startIndex + 1] != 0;
            const uint64_t end = results[endIndex];
            const bool endAvailable = results[endIndex + 1] != 0;

            if (!startAvailable || !endAvailable || end < start) {
                _passTimings.push_back({slot.passNames[static_cast<size_t>(i)], 0.0});
                continue;
            }
            const double ms = static_cast<double>(end - start) *
                static_cast<double>(_timestampPeriod) / 1.0e6;
            _passTimings.push_back({slot.passNames[static_cast<size_t>(i)], ms});
            _frameMilliseconds += ms;
        }

        slot.passCount = 0;
    }
}

#endif // VISUTWIN_HAS_VULKAN
